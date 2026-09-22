/* Host-only production-protocol runner. Not linked into ESP firmware. */
#include "gateway_bacnet.h"
#include "gateway_poll.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static gateway_poll_t published;
static volatile sig_atomic_t stopping;
static gateway_poll_config_t poll_config;
static void stop(int sig) { (void)sig; stopping=1; }
static void *poll_thread(void *unused)
{
    (void)unused;
    gateway_poll_t p;
    gateway_poll_init(&p);
    while(!stopping) {
        gateway_poll_step(&p,&poll_config,mb_monotonic_ms());
        pthread_mutex_lock(&lock); published=p; pthread_mutex_unlock(&lock);
        usleep(25000);
    }
    return NULL;
}

int main(int argc,char **argv)
{
    if(argc!=6) {
        fprintf(stderr,"Usage: %s MODBUS_IPV4 MODBUS_PORT BACNET_LOCAL_IPV4 BACNET_PORT SECONDS\n",argv[0]);
        return 2;
    }
    poll_config=(gateway_poll_config_t){.host=argv[1],.port=(uint16_t)atoi(argv[2]),.unit=41,.expected_firmware=515};
    struct in_addr ip;
    if(inet_pton(AF_INET,argv[3],&ip)!=1) return 2;
    gateway_bacnet_config_t config={
        .device_instance=75181,.device_name="Kohler-MPAC1500-Gateway",
        .firmware_version="0.1.0-host",.location="Native test runner",
        .vendor_id=260,.local_ip=ip.s_addr,.netmask=htonl(0xff000000),
        .udp_port=(uint16_t)atoi(argv[4]),.dhcp_enabled=false,
    };
    signal(SIGINT,stop); signal(SIGTERM,stop);
    gateway_poll_init(&published);
    if(!gateway_bacnet_init(&config,mb_monotonic_ms())) return 1;
    pthread_t thread;
    if(pthread_create(&thread,NULL,poll_thread,NULL)!=0) return 1;
    printf("READY device75181 UDP%s:%u\n",argv[3],config.udp_port); fflush(stdout);
    uint64_t end=mb_monotonic_ms()+(uint64_t)strtoul(argv[5],NULL,10)*1000, next_update=0;
    ats_value_t *values=calloc(ATS_POINT_COUNT,sizeof(*values));
    if(!values) { stopping=1; pthread_join(thread,NULL); return 1; }
    while(!stopping && mb_monotonic_ms()<end) {
        uint64_t now=mb_monotonic_ms();
        if(now>=next_update) {
            ats_model_t model;
            pthread_mutex_lock(&lock); model=published.model; pthread_mutex_unlock(&lock);
            ats_model_decode_all(&model,now,values);
            gateway_bacnet_update(values); next_update=now+100;
        }
        gateway_bacnet_tick(now); gateway_bacnet_poll(10); usleep(1000);
    }
    stopping=1; pthread_join(thread,NULL);
    printf("DONE profile=%s requests=%u successes=%u failures=%u\n",published.profile_status,
           published.requests,published.successes,published.failures);
    free(values); gateway_bacnet_shutdown();
    return 0;
}
