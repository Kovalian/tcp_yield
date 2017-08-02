/*
 * TCP Proportional Integral Derivative (PID)
 * 
 */

#include <linux/module.h>
#include <net/tcp.h>

struct tcpid {
};

static void tcp_pid_init(struct sock *sk) {
    
}

void tcp_pid_pkts_acked(struct sock *sk, u32 cnt, s32 rtt_us) {

}

static void tcp_pid_cong_avoid(struct sock *sk, u32 ack, u32 acked) {

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