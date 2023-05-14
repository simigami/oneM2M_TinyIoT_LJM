#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>
#include "onem2m.h"
#include "jsonparser.h"
#include "dbmanager.h"
#include "httpd.h"
#include "cJSON.h"
#include "util.h"
#include "config.h"
#include "onem2mTypes.h"
#include "mqttClient.h"

ResourceTree *rt;
void route(oneM2MPrimitive *o2pt);
void stop_server(int sig);

int terminate = 0;
int mqtt_error_flag = 0;
char* PORT;

#ifdef ENABLE_MQTT
pthread_t mqtt;
int mqtt_thread_id;
#endif

int main(int argc, char* argv[]) {
	signal(SIGINT, stop_server);
	
	if(!init_dbp()){
		logger("MAIN", LOG_LEVEL_ERROR, "DB Error");
		return 0;
	}
	init_server();
	
	#ifdef ENABLE_MQTT
	mqtt_thread_id = pthread_create(&mqtt, NULL, mqtt_serve, "mqtt Client");
	if(mqtt_thread_id < 0){
		fprintf(stderr, "MQTT thread create error\n");
		return 0;
	}

	pthread_join(mqtt, NULL);

	if(mqtt_error_flag){
		fprintf(stderr, "MQTT Broker Error -> Make Server Terminate\n");
		stop_server(SIGINT);
	}

	#endif

	//if ./server -p [PORT] ? then set port number to PORT
	if(argc >= 3 && strcmp(argv[1], "-p") ==0)
	{
		PORT = argv[2];
	}
	else
	{
		PORT = "3000";
	}

	serve_forever(PORT); // main oneM2M operation logic in void route()    

	return 0;
}

void route(oneM2MPrimitive *o2pt) {
	int rsc = 0;
    double start;

    start = (double)clock() / CLOCKS_PER_SEC; // runtime check - start
	RTNode* target_rtnode = parse_uri(o2pt, rt->cb);
	int e = result_parse_uri(o2pt, target_rtnode);

	if(e != -1) e = check_payload_size(o2pt);
	if(e == -1) {
		respond_to_client(o2pt);
		log_runtime(start);
		return;
	}

	if(o2pt->fc && o2pt->fc->fu != FU_DISCOVERY){
		o2pt->rsc = RSC_BAD_REQUEST;
		set_o2pt_pc(o2pt, "{\"m2m:dbg\":\"Only Filter Usage Discovery Supported\"}");
		respond_to_client(o2pt);
		log_runtime(start);
		return;
	}

	if(o2pt->isFopt)
		rsc = fopt_onem2m_resource(o2pt, target_rtnode);
	else{
		rsc = handle_onem2m_request(o2pt, target_rtnode);
	
		if(o2pt->op != OP_DELETE && target_rtnode->ty == RT_CIN){
			free_rtnode(target_rtnode);
			target_rtnode = NULL;
		}
	}

	respond_to_client(o2pt);
	if(o2pt->op != OP_DELETE && !o2pt->errFlag && target_rtnode) notify_onem2m_resource(o2pt, target_rtnode);
	log_runtime(start);
}

int handle_onem2m_request(oneM2MPrimitive *o2pt, RTNode *target_rtnode){
	int rsc = 0;

	if(o2pt->op == OP_CREATE && o2pt->fc){
		return o2pt->rsc = rsc = RSC_BAD_REQUEST;
	}

	switch(o2pt->op) {
		
		case OP_CREATE:	
			rsc = create_onem2m_resource(o2pt, target_rtnode); break;
		
		case OP_RETRIEVE:
			rsc = retrieve_onem2m_resource(o2pt, target_rtnode); break;
			
		case OP_UPDATE: 
			rsc = update_onem2m_resource(o2pt, target_rtnode); break;
			
		case OP_DELETE:
			rsc = delete_onem2m_resource(o2pt, target_rtnode); break;

		case OP_VIEWER:
			rsc = tree_viewer_api(o2pt, target_rtnode); break;
		
		//case OP_OPTIONS:
			//respond_to_client(200, "{\"m2m:dbg\": \"response about options method\"}", "2000");
			//break;
		case OP_DISCOVERY:
			rsc = discover_onem2m_resource(o2pt, target_rtnode); break;

		default:
			handle_error(o2pt, RSC_INTERNAL_SERVER_ERROR, "{\"m2m:dbg\": \"internal server error\"}");
			return RSC_INTERNAL_SERVER_ERROR;
		}
		
		return rsc;
}

void stop_server(int sig){
	logger("MAIN", LOG_LEVEL_INFO, "Shutting down server...");
	#ifdef ENABLE_MQTT
	terminate = 1;
	logger("MAIN", LOG_LEVEL_INFO, "Waiting for MQTT Client to shut down...");
	pthread_join(mqtt, NULL);
	#endif
	logger("MAIN", LOG_LEVEL_INFO, "Closing DB...");
	close_dbp();
	logger("MAIN", LOG_LEVEL_INFO, "Cleaning ResourceTree...");
	free_all_resource(rt->cb);
	free(rt);
	logger("MAIN", LOG_LEVEL_INFO, "Done");
	exit(0);
}