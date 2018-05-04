/*
 * Yield TCP
 * 
 */

#include <linux/module.h>
#include <net/tcp.h>

#define MIN_CWND 2U

static int min_reduction = 5;
module_param(min_reduction, int, 0644);
MODULE_PARM_DESC(min_reduction, "smallest reduction (largest bit shift) that can be applied to cwnd");

static int max_reduction = 3;
module_param(max_reduction, int, 0644);
MODULE_PARM_DESC(max_reduction, "largest reduction (smallest bit shift) that can be applied to cwnd when no cross traffic is detected");

static int maxc_reduction = 0;
module_param(maxc_reduction, int, 0644);
MODULE_PARM_DESC(max_reduction, "largest reduction (smallest bit shift) that can be applied to cwnd when cross traffic is detected");

static int beta = 15;
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "percentage of total queue capacity to be used as congestion trigger");

static int increase_threshold = 1;
module_param(increase_threshold, int, 0644);
MODULE_PARM_DESC(increase_threshold, "smallest delay increase required over history to reduce "  
    "the reduction factor (trigger a larger decrease)");

static int decrease_threshold = -1;
module_param(decrease_threshold, int, 0644);
MODULE_PARM_DESC(decrease_threshold, "smallest delay decrease required over history to increase "  
    "the reduction factor (trigger a smaller decrease)");

static int hist_factor = 2;
module_param(hist_factor, int, 0644);
MODULE_PARM_DESC(hist_factor, "bit shift applied to average of delay history");

static int trend_factor = 128;
module_param(trend_factor, int, 0644);
MODULE_PARM_DESC(trend_factor, "averaging factor applied to average of delay trend");

static int ct_threshold = 2;
module_param(ct_threshold, int, 0644);
MODULE_PARM_DESC(ct_threshold, "bit shift applied to average of delay history for cross traffic detection");


struct yield {
	u32 delay; /* current delay estimate */
	u32 delay_min; /* propagation delay estimate */
	u32 delay_max; /* maximum delay seen */

	u32 delay_smin; /* smoothed delay minimum */
	u32 delay_smax; /* smoothed delay maximum */

    u32 delay_prev; /* previous delay estimate */
    s32 delay_trend; /* smoothed historic trend */

	s8 reduction_factor; /* bit shift to be applied for window reduction */
    u8 cross_traffic; /* binary value denoting the detection of cross-traffic */

    u32 local_time_offset; /* initial local timestamp for delay estimate */
    u32 remote_time_offset; /* initial remote timestamp for delay estimate */
};

static void tcp_yield_init(struct sock *sk) {
    
    struct yield *yield = inet_csk_ca(sk);

    yield->delay_min = UINT_MAX;
    yield->delay_max = 0;

    yield->delay_smin = yield->delay_smax = 0;

    yield->delay_prev = 0;    
    yield->delay_trend = 0;
    
    yield->reduction_factor = 3;
    yield->cross_traffic = 0;

    yield->local_time_offset = 0;
    yield->remote_time_offset = 0;
}

static u32 update_delay(u32 delay, u32 average, int avg_factor) {

    if (average != 0) {
        delay -= average >> avg_factor; /* delay is now the error in the average */
        average += delay; /* add delay to average as factor-1/factor old + 1/factor new */
    } else {
        average = delay << avg_factor;
    }

    return average;
}

static u32 update_delay_trend(s32 delay, s32 average) {

    if (average != 0) {
        delay -= average / trend_factor; /* delay is now the error in the average */
        average += delay; /* add delay to average as 127/128 old + 1/128 new */
    } else {
        average = delay * trend_factor;
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

void tcp_yield_pkts_acked(struct sock *sk, u32 cnt, s32 rtt_us) {

	struct tcp_sock *tp = tcp_sk(sk);      
    struct yield *yield = inet_csk_ca(sk);

    u32 time, remote_time;
    s32 trend = 0;    

    /* Capture initial timestamps on first run */
    if (yield->remote_time_offset == 0) {
        yield->remote_time_offset = tp->rx_opt.rcv_tsval;
    }    
    if (yield->local_time_offset == 0) {
        yield->local_time_offset = tp->rx_opt.rcv_tsecr;
    }

    time = (tp->rx_opt.rcv_tsval - yield->remote_time_offset) * 1000 / HZ;
    remote_time = (tp->rx_opt.rcv_tsecr - yield->local_time_offset) * 1000 / HZ;

    if (time > remote_time) {
    	yield->delay = time - remote_time;
    }

    /* Update delay_min and delay_max as needed */
    if (yield->delay < yield->delay_min) {
        yield->delay_min = yield->delay;
    } else if (yield->delay > yield->delay_max) {
        yield->delay_max = yield->delay;
    }

    /* Update the smoothed minimum */
    if (((yield->delay_min << 3) < yield->delay_smin) || yield->delay_smin == 0) {
        /* overwrite if the latest minimum is below the smoothed */
        yield->delay_smin = yield->delay_min << 3;
    } else if (yield->delay_min > yield->delay_smin) {
        /* otherwise update the moving average */
        yield->delay_smin = update_delay(yield->delay, yield->delay_smin, 3);
    }

    /* Update the smoothed maximum */
    if (((yield->delay_max << 3) > yield->delay_smax) || yield->delay_smax == 0) {
        /* overwrite if the latest maximum is below the smoothed */
        yield->delay_smax = yield->delay_max << 3;
    } else if (yield->delay_max > yield->delay_smax) {
        /* currently unused */
        /* otherwise update the moving average */
        yield->delay_smax = update_delay(yield->delay, yield->delay_smax, 3);
    }

    if (yield->delay_prev != 0) {
    /* determine whether delay is increasing or decreasing */
        trend = yield->delay - (yield->delay_prev >> hist_factor);

        if (yield->delay_trend != 0 && trend > 
            max((yield->delay_trend / trend_factor) * ct_threshold, 1)) {
                yield->cross_traffic = 1;
        }
    }

    if (trend >= increase_threshold && yield->cross_traffic == 1) {
    /* delay is increasing and cross traffic is present so a bigger decrease will be needed */
        yield->reduction_factor -= 1;
        yield->reduction_factor = max(yield->reduction_factor, maxc_reduction);
    } else if (trend >= increase_threshold) {
    /* delay is increasing so a bigger decrease will be needed */ 
        yield->reduction_factor -= 1;
        yield->reduction_factor = max(yield->reduction_factor, max_reduction);
    } else if (trend <= decrease_threshold) {
    /* delay is decreasing so make the next decrease smaller */
        yield->reduction_factor += 1;
        yield->reduction_factor = min(yield->reduction_factor, min_reduction);
    }

    if (yield->delay < yield->delay_min && yield->delay > 0) {
        yield->delay_min = yield->delay;
    }

    /* current delay reading becomes last seen */
    yield->delay_prev = update_delay(yield->delay, yield->delay_prev, hist_factor);

    /* update delay trend history using current */
    yield->delay_trend = update_delay_trend(trend, yield->delay_trend);

}

static void tcp_yield_cong_avoid(struct sock *sk, u32 ack, u32 acked) {

	struct tcp_sock *tp = tcp_sk(sk);      
    struct yield *yield = inet_csk_ca(sk);

    int target = 0; /* target queuing delay (in ms) */
    u32 qdelay = 0;
    int off_target;

    /* Window under ssthresh, do slow start. */
    if (tp->snd_cwnd <= tp->snd_ssthresh) {
        acked = tcp_slow_start(tp, acked);
        if (!acked)
            return;
    }

    /* Only calculate queuing delay once we have some delay estimates */
    if (yield->delay_smin != 0) {
    	qdelay = yield->delay - (yield->delay_smin >> 3);
    }

    /* Calculate target queuing delay */
    target = beta * 100 * ((yield->delay_smax - yield->delay_smin) >> 3) / 10000;
    
    off_target = target - qdelay;

    if (off_target >= 0) {
    /* under delay target, apply additive increase */
    	tcp_reno_cong_avoid(sk, ack, acked);
    } else {
    /* over delay target, apply multiplicative decrease */
    	u32 decrement;

    	decrement = tp->snd_cwnd >> yield->reduction_factor;
    	tp->snd_cwnd -= decrement;

        /* just decreased, next decrease should be smaller */
        yield->reduction_factor += 1;
    }

    tp->snd_cwnd = max(MIN_CWND, tp->snd_cwnd);

    yield->delay_min = UINT_MAX;
    yield->delay_max = 0;
    yield->cross_traffic = 0;
}

static struct tcp_congestion_ops tcp_yield = {
    .init = tcp_yield_init,
    .ssthresh = tcp_reno_ssthresh,
    .pkts_acked = tcp_yield_pkts_acked,
    .cong_avoid = tcp_yield_cong_avoid,
    .name = "yield"
};

static int __init tcp_yield_register(void){
    BUILD_BUG_ON(sizeof(struct yield) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&tcp_yield);
}

static void __exit tcp_yield_unregister(void){
    tcp_unregister_congestion_control(&tcp_yield);
}

module_init(tcp_yield_register);
module_exit(tcp_yield_unregister);

MODULE_AUTHOR("Kevin Ong");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Yield TCP");
