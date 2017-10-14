/*
 * TCP Proportional Integral Derivative (PID)
 * 
 */

#include <linux/module.h>
#include <net/tcp.h>

#define MIN_CWND 2U

static int min_reduction = 5;
module_param(min_reduction, int, 0644);
MODULE_PARM_DESC(min_reduction, "smallest reduction (largest bit shift) that can be applied to cwnd");

static int max_reduction = 1;
module_param(max_reduction, int, 0644);
MODULE_PARM_DESC(max_reduction, "largest reduction (smallest bit shift) that can be applied to cwnd");

static int beta = 15;
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "percentage of total queue capacity to be used as congestion trigger");

struct tcpid {
	u32 delay; /* current delay estimate */
	u32 delay_min; /* propagation delay estimate */
	u32 delay_max; /* maximum delay seen */

	u32 delay_smin; /* smoothed delay minimum */
	u32 delay_smax; /* smoothed delay maximum */
	u32 delay_tmin; /* time at which the minimum delay was observed */
	u32 delay_tmax; /* time at which the maximum delay was observed */

	u32 window_start; /* start time for most recent adaptation interval */

    u32 delay_prev; /* previous delay estimate */
	s8 reduction_factor; /* bit shift to be applied for window reduction */

    u32 local_time_offset; /* initial local timestamp for delay estimate */
    u32 remote_time_offset; /* initial remote timestamp for delay estimate */
};

static void tcp_pid_init(struct sock *sk) {
    
    struct tcpid *pid = inet_csk_ca(sk);

    pid->delay_min = UINT_MAX;
    pid->delay_max = 0;

    pid->delay_smin = pid->delay_smax = 0;
    pid->delay_tmin = pid->delay_tmax = 0;

    pid->delay_prev = 0;    
    pid->reduction_factor = 3;

    pid->local_time_offset = 0;
    pid->remote_time_offset = 0;
}

static u32 update_delay(u32 delay, u32 average) {

    if (average != 0) {
        delay -= average >> 3; /* delay is now the error in the average */
        average += delay; /* add delay to average as 7/8 old + 1/8 new */
    } else {
        average = delay << 3;
    }

    return average;
}

static inline u32 time_in_ms(void)
{
    #if HZ < 1000
        return ktime_to_ms(ktime_get_real());
    #else
        return jiffies_to_msecs(jiffies);
    #endif
}

void tcp_pid_pkts_acked(struct sock *sk, u32 cnt, s32 rtt_us) {

	struct tcp_sock *tp = tcp_sk(sk);      
    struct tcpid *pid = inet_csk_ca(sk);

    u32 time, remote_time;
    s32 trend = 0;    

    /* Capture initial timestamps on first run */
    if (pid->remote_time_offset == 0) {
        pid->remote_time_offset = tp->rx_opt.rcv_tsval;
    }    
    if (pid->local_time_offset == 0) {
        pid->local_time_offset = tp->rx_opt.rcv_tsecr;
    }

    time = (tp->rx_opt.rcv_tsval - pid->remote_time_offset) * 1000 / HZ;
    remote_time = (tp->rx_opt.rcv_tsecr - pid->local_time_offset) * 1000 / HZ;

    if (time > remote_time) {
    	pid->delay = time - remote_time;
    }

    /* Update delay_min and delay_max as needed */
    if (pid->delay < pid->delay_min) {
        pid->delay_min = pid->delay;
        pid->delay_tmin = tp->rx_opt.rcv_tsval;
    } else if (pid->delay > pid->delay_max) {
        pid->delay_max = pid->delay;
        pid->delay_tmax = tp->rx_opt.rcv_tsval;
    }

    /* Update the smoothed minimum */
    if ((pid->delay_min << 3) < pid->delay_smin) {
        /* overwrite if the latest minimum is below the smoothed */
        pid->delay_smin = pid->delay_min << 3;
    } else if (pid->delay_min > pid->delay_smin) {
        /* otherwise update the moving average */
        pid->delay_smin = update_delay(pid->delay, pid->delay_smin);
    }

    /* Update the smoothed maximum */
    if ((pid->delay_max << 3) < pid->delay_smax) {
        /* overwrite if the latest maximum is below the smoothed */
        pid->delay_smax = pid->delay_max << 3;
    } else if (pid->delay_max > pid->delay_smax) {
        /* otherwise update the moving average */
        pid->delay_smax = update_delay(pid->delay, pid->delay_smax);
    }

    if (pid->delay_prev != 0) {
    /* determine whether delay is increasing or decreasing */
        trend = pid->delay - pid->delay_prev;
    }

    if (trend > 1) {
    /* delay is increasing so a bigger decrease will be needed */ 
        pid->reduction_factor -= 1;
        pid->reduction_factor = max(pid->reduction_factor, max_reduction);
    } else if (trend < -1) {
    /* delay is decreasing so make the next decrease smaller */
        pid->reduction_factor += 1;
        pid->reduction_factor = min(pid->reduction_factor, min_reduction);
    }

    if (pid->delay < pid->delay_min && pid->delay > 0) {
        pid->delay_min = pid->delay;
    }

    /* current delay reading becomes last seen */
    pid->delay_prev = pid->delay;

}

static void tcp_pid_cong_avoid(struct sock *sk, u32 ack, u32 acked) {

	struct tcp_sock *tp = tcp_sk(sk);      
    struct tcpid *pid = inet_csk_ca(sk);

    int target = 0; /* target queuing delay (in ms) */
    u32 trigger_delay = 0;
    u32 qdelay = 0;
    int off_target;

    /* Window under ssthresh, do slow start. */
    if (tp->snd_cwnd <= tp->snd_ssthresh) {
        acked = tcp_slow_start(tp, acked);
        pid->window_start = time_in_ms();
        if (!acked)
            return;
    }

    /* Only calculate queuing delay once we have some delay estimates */
    if (pid->delay_smin != 0) {
    	qdelay = pid->delay - pid->delay_smin;
    }

    /* Calculate target queuing delay */
	target = beta * 100 * (pid->delay_smax - pid->delay_smin) / 10000;

    off_target = target - qdelay;

    if (off_target >= 0) {
    /* under delay target, apply additive increase */
    	tcp_reno_cong_avoid(sk, ack, acked);
    } else {
    /* over delay target, apply multiplicative decrease */
    	u32 decrement;

    	decrement = tp->snd_cwnd >> pid->reduction_factor;
    	tp->snd_cwnd -= decrement;
    }

    tp->snd_cwnd = max(MIN_CWND, tp->snd_cwnd);

    /* Check for end of adaptation interval */
    if (time_in_ms() >= pid->window_start + (pid->delay_tmax - pid->delay_tmin)) {
    /* Reset all readings for end of adaptation interval */
        pid->delay_min = UINT_MAX;
        pid->delay_max = 0;
        pid->delay_smin = pid->delay_smax = 0;
        pid->reduction_factor = 3; /* reset reduction factor to 1/8 */ 
    }
  
}

static struct tcp_congestion_ops tcp_pid = {
    .init = tcp_pid_init,
    .ssthresh = tcp_reno_ssthresh,
    .pkts_acked = tcp_pid_pkts_acked,
    .cong_avoid = tcp_pid_cong_avoid,
    .name = "pid"
};

static int __init tcp_pid_register(void){
    BUILD_BUG_ON(sizeof(struct tcpid) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&tcp_pid);
}

static void __exit tcp_pid_unregister(void){
    tcp_unregister_congestion_control(&tcp_pid);
}

module_init(tcp_pid_register);
module_exit(tcp_pid_unregister);

MODULE_AUTHOR("Kevin Ong");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP PID");