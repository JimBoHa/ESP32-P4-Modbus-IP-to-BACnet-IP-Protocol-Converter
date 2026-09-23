/* Production handlers with in-memory IDF adapters. Signature acceptance is
 * injected here; cryptography is verified separately during the signed build.
 * No sockets, actual flash writes, or production credentials. */
#include "gateway_ota.h"
#include "ota_auth.h"
#include "ota_health.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_https_server.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define ADMIN "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define VIEWER "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv"
#define PROJECT "esp32_p4_bacnet_switches"
#ifdef __APPLE__
#define CONSTANT_SECTION ".section __TEXT,__const\n"
#else
#define CONSTANT_SECTION ".section .rodata\n"
#endif
__asm__(CONSTANT_SECTION
    ".global _binary_ota_server_cert_pem_start\n_binary_ota_server_cert_pem_start:\n.asciz \"synthetic-cert\"\n"
    ".global _binary_ota_server_cert_pem_end\n_binary_ota_server_cert_pem_end:\n"
    ".global _binary_ota_server_key_pem_start\n_binary_ota_server_key_pem_start:\n.asciz \"synthetic-key\"\n"
    ".global _binary_ota_server_key_pem_end\n_binary_ota_server_key_pem_end:\n"
    ".global _binary_ota_token_txt_start\n_binary_ota_token_txt_start:\n.asciz \"" ADMIN "\"\n"
    ".global _binary_ota_token_txt_end\n_binary_ota_token_txt_end:\n"
    ".global _binary_ota_viewer_token_txt_start\n_binary_ota_viewer_token_txt_start:\n.asciz \"" VIEWER "\"\n"
    ".global _binary_ota_viewer_token_txt_end\n_binary_ota_viewer_token_txt_end:\n.text\n");
static esp_partition_t running={.type=0,.subtype=0x11,.address=0x420000,.size=0x400000,.label="ota_1"};
static const esp_partition_t target={.type=0,.subtype=0x10,.address=0x20000,.size=0x400000,.label="ota_0"};
static esp_app_desc_t descriptor={.secure_version=2,.version="0.3.0",.project_name=PROJECT,.idf_ver="v5.5.4"};
static esp_app_desc_t candidate={.secure_version=2,.version="0.3.1",.project_name=PROJECT,.idf_ver="v5.5.4"};
static esp_ota_img_states_t current_state=ESP_OTA_IMG_VALID;
static httpd_uri_t routes[4];
static unsigned route_count,begins,writes,ends,aborts,selected,restarts,validated;
static size_t written_bytes;
static bool no_target,same_target,no_running,bootstrap;
static esp_err_t state_error,begin_error,write_error,end_error,hash_error,select_error,valid_error;
static esp_err_t register_error,nvs_error;
static int tls_error;
static unsigned signature_keys=1,ssl_stops,web_registered,health_samples,early_samples;
static uint32_t stored_boots=10;
static int64_t fake_us=1000000,io_advance;
static struct fake_timer {void (*callback)(void *);void *arg;bool active;int64_t due;} deadline_timer;
static void (*created_task)(void *);
static void *created_task_arg;
static bool task_fail,health_good=true,fail_one_sample,force_jump;
static jmp_buf reset_jump;
static char image[1024];

void esp_restart(void){++restarts;if(force_jump)longjmp(reset_jump,1);}
void esp_fill_random(void *out,size_t size){memset(out,0x55,size);}
int64_t esp_timer_get_time(void){return fake_us;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *t)
{deadline_timer.callback=a->callback;deadline_timer.arg=a->arg;*t=&deadline_timer;return ESP_OK;}
esp_err_t esp_timer_start_once(esp_timer_handle_t t,uint64_t time)
{assert(t==&deadline_timer&&time==60000000);t->active=true;t->due=fake_us+time;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t t){assert(t==&deadline_timer);t->active=false;return ESP_OK;}
esp_err_t esp_timer_delete(esp_timer_handle_t t){assert(t==&deadline_timer);t->active=false;return ESP_OK;}
void vTaskDelay(uint32_t ticks)
{fake_us+=(int64_t)ticks*1000;if(deadline_timer.active&&fake_us>=deadline_timer.due){deadline_timer.active=false;deadline_timer.callback(deadline_timer.arg);}}
void vTaskDelete(void *task){assert(task==NULL);}
BaseType_t xTaskCreate(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{(void)name;(void)stack;(void)priority;(void)handle;if(task_fail)return 0;created_task=task;created_task_arg=arg;return pdPASS;}
void mbedtls_pk_init(mbedtls_pk_context *k){k->unused=0;}
void mbedtls_pk_free(mbedtls_pk_context *k){(void)k;}
int mbedtls_pk_parse_key(mbedtls_pk_context *k,const unsigned char *d,size_t l,const unsigned char *p,size_t pl,int (*r)(void *,unsigned char *,size_t),void *a)
{(void)k;(void)d;(void)l;(void)p;(void)pl;(void)r;(void)a;return tls_error;}
int mbedtls_pk_check_pair(const mbedtls_pk_context *a,const mbedtls_pk_context *b,int (*r)(void *,unsigned char *,size_t),void *v)
{(void)a;(void)b;(void)r;(void)v;return tls_error;}
void mbedtls_x509_crt_init(mbedtls_x509_crt *c){c->pk.unused=0;}
void mbedtls_x509_crt_free(mbedtls_x509_crt *c){(void)c;}
int mbedtls_x509_crt_parse(mbedtls_x509_crt *c,const unsigned char *d,size_t s)
{(void)c;(void)d;(void)s;return tls_error;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h)
{assert(!strcmp(name,"gw_system")&&mode==NVS_READWRITE);if(nvs_error)return nvs_error;*h=1;return ESP_OK;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *v)
{assert(h==1&&!strcmp(key,"boots"));*v=stored_boots;return ESP_OK;}
esp_err_t nvs_set_u32(nvs_handle_t h,const char *key,uint32_t v)
{assert(h==1&&!strcmp(key,"boots"));stored_boots=v;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);return ESP_OK;}
void nvs_close(nvs_handle_t h){assert(h==1);}
const esp_app_desc_t *esp_app_get_description(void){return &descriptor;}
const esp_partition_t *esp_ota_get_running_partition(void){return no_running?NULL:&running;}
const esp_partition_t *esp_ota_get_boot_partition(void){return selected?&target:&running;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *last)
{assert(last==NULL);return no_target?NULL:same_target?&running:&target;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s)
{assert(p==&running);if(state_error)return state_error;*s=current_state;return ESP_OK;}
esp_err_t esp_partition_get_sha256(const esp_partition_t *p,uint8_t *hash)
{assert(p==&running||p==&target);if(hash_error)return hash_error;memset(hash,p==&running?0x11:0x22,32);return ESP_OK;}
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool digest,esp_image_sig_public_key_digests_t *keys)
{assert(digest);keys->num_digests=signature_keys;memset(keys->key_digests,0x44,sizeof(keys->key_digests));return ESP_OK;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h)
{assert(p==&target&&size>=288&&size<=target.size);++begins;if(begin_error)return begin_error;*h=1;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *d,size_t size)
{assert(h==1&&d&&size&&size<=4096);++writes;written_bytes+=size;return write_error;}
esp_err_t esp_ota_end(esp_ota_handle_t h){assert(h==1);++ends;return end_error;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){assert(h==1);++aborts;return ESP_OK;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *a)
{assert(p==&target);*a=candidate;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{assert(bootstrap?p==&running:p==&target&&ends==1);if(select_error)return select_error;++selected;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{assert(current_state==ESP_OTA_IMG_PENDING_VERIFY);++validated;if(valid_error)return valid_error;current_state=ESP_OTA_IMG_VALID;return ESP_OK;}
esp_err_t httpd_ssl_start(httpd_handle_t *h,const httpd_ssl_config_t *c)
{assert(c->port_secure==443&&c->httpd.max_open_sockets==2&&c->servercert&&c->prvtkey_pem);*h=routes;return ESP_OK;}
esp_err_t httpd_ssl_stop(httpd_handle_t h){assert(h==routes);++ssl_stops;route_count=0;return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t *u)
{assert(h==routes&&route_count<4);if(register_error)return register_error;routes[route_count++]=*u;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *v){snprintf(r->response_type,sizeof(r->response_type),"%s",v);return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *v){snprintf(r->status,sizeof(r->status),"%s",v);return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v){(void)r;assert(k&&v);return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t *r,const char *b,ssize_t l)
{assert(!r->response_sent);if(l==-1)l=strlen(b);assert(l>=0);r->response=malloc(l+1);assert(r->response);memcpy(r->response,b,l);r->response[l]=0;r->response_sent=true;return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *m)
{snprintf(r->status,sizeof(r->status),"%d Error",code);return httpd_resp_send(r,m,-1);}
static const char *header(httpd_req_t *r,const char *k)
{return !strcmp(k,"Authorization")?r->authorization:!strcmp(k,"Content-Type")?r->content_type:!strcmp(k,"X-Firmware-Project")?r->project:NULL;}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *k){const char *v=header(r,k);return v?strlen(v):0;}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *k,char *o,size_t s)
{const char *v=header(r,k);if(!v||strlen(v)>=s)return ESP_FAIL;snprintf(o,s,"%s",v);return ESP_OK;}
int httpd_req_recv(httpd_req_t *r,char *out,size_t size)
{fake_us+=io_advance;if(r->receive_timeouts){--r->receive_timeouts;return HTTPD_SOCK_ERR_TIMEOUT;}
 if(size>r->body_length-r->received)size=r->body_length-r->received;if(r->chunk_size&&size>r->chunk_size)size=r->chunk_size;
 memcpy(out,r->body+r->received,size);r->received+=size;return (int)size;}
static esp_err_t register_web(httpd_handle_t h){assert(h==routes);++web_registered;return ESP_OK;}
static bool healthy(void)
{++health_samples;if(fake_us<11000000)++early_samples;if(fail_one_sample&&health_samples==4)return false;return health_good;}
static httpd_req_t request(const char *path,int method)
{return (httpd_req_t){.uri=path,.method=method,.body=image,.body_length=sizeof(image),.content_len=sizeof(image),
 .chunk_size=53,.authorization="Bearer " ADMIN,.content_type="application/octet-stream",.project=PROJECT,.status="200 OK"};}
static void dispatch(httpd_req_t *r,unsigned status)
{for(unsigned i=0;i<route_count;++i)if(routes[i].method==r->method&&!strcmp(routes[i].uri,r->uri)){
 (void)routes[i].handler(r);if(!r->response_sent||(unsigned)atoi(r->status)!=status)fprintf(stderr,"%s expected%u got%s\n",r->uri,status,r->status);
 assert(r->response_sent&&(unsigned)atoi(r->status)==status);return;}assert(false);}
static void release(httpd_req_t *r){free(r->response);}
static void denied(httpd_req_t *r,unsigned s){dispatch(r,s);assert(!selected&&!restarts);release(r);}
static void test_auth(void)
{
 httpd_req_t r=request("/ota",HTTP_POST);r.authorization=NULL;denied(&r,401);
 r=request("/ota",HTTP_POST);r.authorization="Bearer " VIEWER;denied(&r,403);
 r=request("/ota",HTTP_POST);r.authorization="Bearer invalid";denied(&r,401);
 r=request("/ota",HTTP_POST);current_state=ESP_OTA_IMG_PENDING_VERIFY;denied(&r,409);
 r=request("/ota",HTTP_POST);current_state=ESP_OTA_IMG_VALID;state_error=ESP_FAIL;denied(&r,500);
 state_error=0;r=request("/ota",HTTP_POST);gateway_ota_note_restart_pending();denied(&r,409);assert(!begins);
}
static void test_headers(void)
{
 httpd_req_t r=request("/ota",HTTP_POST);r.project="different";denied(&r,400);
 r=request("/ota",HTTP_POST);r.project=NULL;denied(&r,400);
 r=request("/ota",HTTP_POST);r.content_type="text/plain";denied(&r,415);
 r=request("/ota",HTTP_POST);r.content_len=287;denied(&r,400);
 r=request("/ota",HTTP_POST);r.content_len=0x400001;denied(&r,413);
 r=request("/ota",HTTP_POST);no_target=true;denied(&r,500);no_target=false;
 r=request("/ota",HTTP_POST);same_target=true;denied(&r,500);assert(!begins);
}
static void test_truncated(void){httpd_req_t r=request("/ota",HTTP_POST);r.body_length=300;denied(&r,400);assert(begins==1&&aborts==1&&!ends);}
static void test_deadline(void){httpd_req_t r=request("/ota",HTTP_POST);io_advance=300000000;denied(&r,408);assert(aborts==1);}
static void test_timeout(void){httpd_req_t r=request("/ota",HTTP_POST);r.receive_timeouts=6;denied(&r,400);assert(aborts==1&&!writes);}
static void test_begin_fail(void){httpd_req_t r=request("/ota",HTTP_POST);begin_error=ESP_FAIL;denied(&r,500);assert(!aborts&&!ends);}
static void test_write_fail(void){httpd_req_t r=request("/ota",HTTP_POST);write_error=ESP_ERR_OTA_VALIDATE_FAILED;denied(&r,400);assert(aborts==1&&!ends);}
static void test_signature_fail(void){httpd_req_t r=request("/ota",HTTP_POST);end_error=ESP_ERR_OTA_VALIDATE_FAILED;denied(&r,400);assert(ends==1&&!aborts);}
static void test_project_fail(void){httpd_req_t r=request("/ota",HTTP_POST);strcpy(candidate.project_name,"foreign");denied(&r,400);assert(ends==1);}
static void test_secure_version(void){httpd_req_t r=request("/ota",HTTP_POST);candidate.secure_version=1;denied(&r,400);}
static void test_descriptor(void){httpd_req_t r=request("/ota",HTTP_POST);memset(candidate.version,'x',sizeof(candidate.version));denied(&r,400);}
static void test_hash(void){httpd_req_t r=request("/ota",HTTP_POST);hash_error=ESP_FAIL;denied(&r,500);}
static void test_select(void){httpd_req_t r=request("/ota",HTTP_POST);select_error=ESP_FAIL;denied(&r,500);}
static void test_upload_ok(void)
{
 httpd_req_t r=request("/ota",HTTP_POST);r.receive_timeouts=5;dispatch(&r,202);
 assert(begins==1&&ends==1&&selected==1&&restarts==1&&!aborts&&written_bytes==sizeof(image));
 cJSON *j=cJSON_Parse(r.response);assert(j&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"accepted")));
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"rebooting")));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"partition")->valuestring,"ota_0"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"version")->valuestring,"0.3.1"));
 assert(strlen(cJSON_GetObjectItemCaseSensitive(j,"image_sha256")->valuestring)==64);cJSON_Delete(j);release(&r);
}
static void test_status(void)
{
 httpd_req_t r=request("/ota/status",HTTP_GET);r.authorization=NULL;dispatch(&r,200);
 cJSON *j=cJSON_Parse(r.response);assert(j);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"project")->valuestring,PROJECT));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"version")->valuestring,"0.3.0"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"state")->valuestring,"valid"));
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(j,"partition")->valuestring,"ota_1"));
 assert(strlen(cJSON_GetObjectItemCaseSensitive(j,"image_sha256")->valuestring)==64);
 cJSON *s=cJSON_GetObjectItemCaseSensitive(j,"security"),*p=cJSON_GetObjectItemCaseSensitive(j,"ota_policy");
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s,"software_signature_verification")));
 assert(strlen(cJSON_GetObjectItemCaseSensitive(p,"signing_key_sha256")->valuestring)==64);
 assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"system"),"boot_count")->valuedouble==11);
 assert(!strstr(r.response,ADMIN)&&!strstr(r.response,VIEWER)&&!strstr(r.response,"synthetic-key"));cJSON_Delete(j);release(&r);
 r=request("/ota/status",HTTP_GET);r.authorization="Bearer invalid";dispatch(&r,401);release(&r);
 r=request("/ota/status",HTTP_GET);r.authorization="Bearer " VIEWER;dispatch(&r,200);release(&r);
}
static void test_reboot(void)
{
 httpd_req_t r=request("/system/reboot",HTTP_POST);dispatch(&r,400);release(&r);assert(!restarts);
 r=request("/system/reboot",HTTP_POST);r.content_len=0;dispatch(&r,202);
 cJSON *j=cJSON_Parse(r.response);assert(j&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"accepted")));
 assert(cJSON_GetObjectItemCaseSensitive(j,"boot_count")->valuedouble==11);
 assert(strlen(cJSON_GetObjectItemCaseSensitive(j,"image_sha256")->valuestring)==64);cJSON_Delete(j);release(&r);assert(restarts==1&&!begins);
}
static void test_health_ok(void)
{
 current_state=ESP_OTA_IMG_PENDING_VERIFY;
 assert(gateway_ota_begin_validation(healthy)==ESP_OK&&deadline_timer.active&&created_task);
 fail_one_sample=true;created_task(created_task_arg);
 assert(validated==1&&current_state==ESP_OTA_IMG_VALID&&health_samples==9&&!early_samples&&!deadline_timer.active&&!restarts);
}
static void test_health_bad(void)
{
 current_state=ESP_OTA_IMG_PENDING_VERIFY;health_good=false;assert(gateway_ota_begin_validation(healthy)==ESP_OK);force_jump=true;
 if(setjmp(reset_jump)==0){created_task(created_task_arg);assert(false);}
 assert(restarts==1&&!validated&&current_state==ESP_OTA_IMG_PENDING_VERIFY&&fake_us>=61000000);
}
static void test_stall(void)
{
 current_state=ESP_OTA_IMG_PENDING_VERIFY;assert(gateway_ota_begin_validation(healthy)==ESP_OK);force_jump=true;
 if(setjmp(reset_jump)==0){vTaskDelay(60000);assert(false);}assert(restarts==1&&!validated&&!health_samples);
}
static void test_task_fail(void)
{
 current_state=ESP_OTA_IMG_PENDING_VERIFY;task_fail=true;
 assert(gateway_ota_begin_validation(healthy)==ESP_ERR_NO_MEM&&deadline_timer.active);force_jump=true;
 if(setjmp(reset_jump)==0){vTaskDelay(60000);assert(false);}assert(restarts==1&&!validated);
}
static void test_bootstrap(void)
{
 state_error=ESP_ERR_NOT_FOUND;bootstrap=true;
 assert(gateway_ota_begin_validation(healthy)==ESP_OK);assert(selected==1&&restarts==1&&!validated&&!created_task);
}
static void test_bootstrap_bad(void)
{
 state_error=ESP_ERR_NOT_FOUND;bootstrap=true;no_target=true;
 assert(gateway_ota_begin_validation(healthy)==ESP_ERR_INVALID_STATE);assert(!selected&&!restarts);
}
static void test_bootstrap_signature_failure(void)
{
 state_error=ESP_ERR_NOT_FOUND;bootstrap=true;select_error=ESP_ERR_OTA_VALIDATE_FAILED;
 assert(gateway_ota_begin_validation(healthy)==ESP_ERR_OTA_VALIDATE_FAILED);
 assert(!selected&&!restarts&&!validated);
}
static void test_validation_failure(void)
{
 current_state=ESP_OTA_IMG_PENDING_VERIFY;valid_error=ESP_FAIL;
 assert(gateway_ota_begin_validation(healthy)==ESP_OK);
 created_task(created_task_arg);
 assert(validated==1&&restarts==1&&current_state==ESP_OTA_IMG_PENDING_VERIFY);
}
static void test_models(void)
{
 assert(ota_role_tokens_valid(ADMIN,VIEWER)&&!ota_role_tokens_valid(ADMIN,ADMIN));
 assert(ota_authorization_role(NULL,0,ADMIN,VIEWER)==OTA_ROLE_ANONYMOUS);
 assert(!ota_role_allows(OTA_ROLE_ANONYMOUS,false)&&ota_role_allows(OTA_ROLE_ANONYMOUS,true));
 assert(ota_authorization_role("Bearer " ADMIN,39,ADMIN,VIEWER)==OTA_ROLE_ADMIN);
 assert(ota_authorization_role("bearer " ADMIN,39,ADMIN,VIEWER)==OTA_ROLE_NONE);
 assert(ota_authorization_role("Bearer " VIEWER,39,ADMIN,VIEWER)==OTA_ROLE_VIEWER);
 char copy[129];assert(ota_copy_embedded_token((const uint8_t *)ADMIN "\n",33,copy,sizeof(copy))&&!strcmp(copy,ADMIN));
 assert(!ota_copy_embedded_token((const uint8_t *)"bad",3,copy,sizeof(copy)));
 ota_health_gate_t g={0};assert(!ota_health_gate_sample(&g,true,5));assert(!ota_health_gate_sample(&g,false,5)&&!g.consecutive_healthy_samples);
}
static void isolated(void (*test)(void),const char *name)
{
 fflush(NULL);pid_t pid=fork();assert(pid>=0);if(!pid){test();_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid);if(!WIFEXITED(status)||WEXITSTATUS(status))fprintf(stderr,"FAILED %s\n",name);
 assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
#define RUN(test) isolated(test,#test)
int main(void)
{
 test_models();assert(!gateway_ota_ready());
 tls_error=-1;assert(gateway_ota_start(register_web)==ESP_ERR_INVALID_ARG&&!gateway_ota_ready());tls_error=0;
 signature_keys=0;assert(gateway_ota_start(register_web)==ESP_ERR_INVALID_STATE&&!gateway_ota_ready());signature_keys=1;
 nvs_error=ESP_FAIL;assert(gateway_ota_start(register_web)==ESP_FAIL&&!gateway_ota_ready());nvs_error=0;
 register_error=ESP_FAIL;assert(gateway_ota_start(register_web)==ESP_FAIL&&ssl_stops==1&&!gateway_ota_ready());register_error=0;
 stored_boots=10;assert(gateway_ota_start(register_web)==ESP_OK&&gateway_ota_ready()&&web_registered==1&&route_count==3);
 RUN(test_auth);RUN(test_headers);RUN(test_truncated);RUN(test_deadline);RUN(test_timeout);
 RUN(test_begin_fail);RUN(test_write_fail);RUN(test_signature_fail);RUN(test_project_fail);RUN(test_secure_version);
 RUN(test_descriptor);RUN(test_hash);RUN(test_select);RUN(test_upload_ok);RUN(test_status);RUN(test_reboot);
 RUN(test_health_ok);RUN(test_health_bad);RUN(test_stall);RUN(test_task_fail);RUN(test_bootstrap);RUN(test_bootstrap_bad);RUN(test_bootstrap_signature_failure);RUN(test_validation_failure);
 puts("OTA:24 isolated production-handler cases plus startup/auth/health-model checks passed; no network/flash I/O");
 return 0;
}
