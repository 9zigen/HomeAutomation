/*
RFM69 Gateway RFM69 pushing the data to the mosquitto server
by Alexandre Bouillot

License:  CC-BY-SA, https://creativecommons.org/licenses/by-sa/2.0/
Date:  2014-04-13
File: Gateway.c

This sketch receives RFM wireless data and forwards it to Mosquitto relay
It also subscripe to Mosquitto Topics starting with RFM/<network_number> followed  by /<node_id>
The message is parsed and put bak to the same payload structure as the one received from teh nodes


Modifications Needed:
1)  Update encryption string "ENCRYPTKEY"
2)  Adjust network id, node id, frequency and model of RFM
*/

//general --------------------------------
#define SERIAL_BAUD   115200
#ifdef DEBUG
#define DEBUG1(expression)  fprintf(stderr, expression)
#define DEBUG2(expression, arg)  fprintf(stderr, expression, arg)
#define DEBUGLN1(expression)  
#ifdef DAEMON
#define LOG(...) do { syslog(LOG_INFO, __VA_ARGS__); } while (0)
#define LOG_E(...) do { syslog(LOG_ERR, __VA_ARGS__); } while (0)
#else
#define LOG(...) do { printf(__VA_ARGS__); } while (0)
#define LOG_E(...) do { printf(__VA_ARGS__); } while (0)
#endif //DAEMON
#else
#define DEBUG1(expression)
#define DEBUG2(expression, arg)
#define DEBUGLN1(expression)
#define LOG(...)
#define LOG_E(...)
#endif

//RFM69  ----------------------------------
#include "rfm69.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

RFM69 *rfm69;

typedef struct {		
	unsigned long messageWatchdog;
	unsigned long messageSent;
	unsigned long messageReceived;
	unsigned long ackRequested;
	
	unsigned long ackReceived;
	unsigned long ackMissed;
	
	unsigned long ackCount;
} 
Stats;
Stats theStats;

typedef struct {
	uint8_t networkId;
	uint8_t nodeId;
	uint8_t frequency; // RF69_433MHZ RF69_868MHZ RF69_915MHZ
	uint8_t keyLength; // set to 0 for no encryption
	char key[16];
	bool isRFM69HW;
	bool promiscuousMode;
	unsigned long messageWatchdogDelay; // maximum time between two message before restarting radio module
	}
Config;
Config theConfig;

// Mosquitto---------------
#include <mosquitto.h>

/* How many seconds the broker should wait between sending out
* keep-alive messages. */
#define KEEPALIVE_SECONDS 60
/* Hostname and port for the MQTT broker. */
#define BROKER_HOSTNAME "localhost"
#define BROKER_PORT 1883

#define MQTT_ROOT "RFM"
#define MQTT_CLIENT_ID "arduinoClient"
#define MQTT_RETRY 500

int sendMQTT = 0;

typedef struct {		
	short           nodeID; 
	short			sensorID;
	unsigned long   var1_usl; 
	float           var2_float; 
	float			var3_float;	
} 
Payload;
Payload theData;

typedef struct {
	short           nodeID;
	short			sensorID;		
	unsigned long   var1_usl;
	float           var2_float;
	float			var3_float;		//
	int             var4_int;
}
SensorNode;
SensorNode sensorNode;

static void die(const char *msg);
static long millis(void);
static int initRfm(RFM69 *rfm);

static bool set_callbacks(struct mosquitto *m);
static bool connect(struct mosquitto *m);
static int run_loop(struct mosquitto *m);

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val);
static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val);
static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val);

static void uso(void) {
	fprintf(stderr, "Use:\n Simply use it without args :D\n");
	exit(1);
}

int main(int argc, char* argv[]) {
	if (argc != 1) uso();

#ifdef DAEMON
	//Adapted from http://www.netzmafia.de/skripten/unix/linux-daemon-howto.html
	pid_t pid, sid;

	openlog("Gatewayd", 0, LOG_USER);

	pid = fork();
	if (pid < 0) {
		LOG_E("fork failed");
		exit(EXIT_FAILURE);
	}
	/* If we got a good PID, then
		 we can exit the parent process. */
	if (pid > 0) {
		LOG("Child spawned, pid %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		LOG_E("setsid failed");
		exit(EXIT_FAILURE);
	}
        
	/* Change the current working directory */
	if ((chdir("/")) < 0) {
	  LOG_E("chdir failed");
	  exit(EXIT_FAILURE);
	}
        
	/* Close out the standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
#endif //DAEMON

	// Mosquitto ----------------------
	struct mosquitto *m = mosquitto_new(MQTT_CLIENT_ID, true, null);
	if (m == NULL) { die("init() failure\n"); }

	if (!set_callbacks(m)) { die("set_callbacks() failure\n"); }
	if (!connect(m)) { die("connect() failure\n"); }

	//RFM69 ---------------------------
	theConfig.networkId = 101;
	theConfig.nodeId = 1;
	theConfig.frequency = RF69_433MHZ;
	theConfig.keyLength = 16;
	memcpy(theConfig.key, "xxxxxxxxxxxxxxxx", 16);
	theConfig.isRFM69HW = true;
	theConfig.promiscuousMode = false;
	theConfig.messageWatchdogDelay = 1800000; // 1800 seconds (30 minutes) between two messages 

	rfm69 = new RFM69();
	rfm69->initialize(theConfig.frequency,theConfig.nodeId,theConfig.networkId);
	initRfm(rfm69);

	// Mosquitto subscription ---------
	char subsciptionMask[128];
	sprintf(subsciptionMask, "%s/%03d/#", MQTT_ROOT, theConfig.networkId);
	LOG("Subscribe to Mosquitto topic: %s\n", subsciptionMask);
	mosquitto_subscribe(m, NULL, subsciptionMask, 0);
	
	LOG("setup complete\n");
	return run_loop(m);
}  // end of setup

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct mosquitto *m) {
	int res;
	long lastMess; 
	for (;;) {
		res = mosquitto_loop(m, 10, 1);

//		printf("%d dB\n",rfm69->readRSSI());
		
		// No messages have been received withing MESSAGE_WATCHDOG interval
		if (millis() > lastMess + theConfig.messageWatchdogDelay) {
			LOG("=== Message WatchDog ===\n");
			theStats.messageWatchdog++;
			// re-initialise the radio
			initRfm(rfm69);
			// reset watchdog
			lastMess = millis();
		}
		
		if (rfm69->receiveDone()) {
			// record last message received time - to compute radio watchdog
			lastMess = millis();
			
			theStats.messageReceived++;

			LOG("[%d] ",rfm69->SENDERID);
			if (theConfig.promiscuousMode) {
				LOG(" to [%d] ", rfm69->TARGETID);
			}

			if (rfm69->DATALEN != sizeof(Payload)) {
				LOG("Invalid payload received, not matching Payload struct! %d - %d\r\n", rfm69->DATALEN, sizeof(Payload));
				for(int i = 0; i < rfm69->DATALEN; i++) {
					LOG("%02x.", rfm69->DATA[i]); }
				LOG("\n");			
				}
			else {
				theData = *(Payload*)rfm69->DATA; //assume radio.DATA actually contains our struct and not something else

				//save it for i2c:
				sensorNode.nodeID = theData.nodeID;
				sensorNode.sensorID = theData.sensorID;
				sensorNode.var1_usl = theData.var1_usl;
				sensorNode.var2_float = theData.var2_float;
				sensorNode.var3_float = theData.var3_float;
				sensorNode.var4_int = rfm69->RSSI;

				LOG("Received Node ID = %d Device ID = %d Time = %d  RSSI = %d var2 = %f var3 = %f\n",
					sensorNode.nodeID,
					sensorNode.sensorID,
					sensorNode.var1_usl,
					sensorNode.var4_int,
					sensorNode.var2_float,
					sensorNode.var3_float
				);
				sendMQTT = 1;
			}

			if (rfm69->ACK_REQUESTED) {
				theStats.ackRequested++;
				uint8_t theNodeID = rfm69->SENDERID;
				rfm69->sendACK();
				// When a node requests an ACK, respond to the ACK
				// and also send a packet requesting an ACK (every 3rd one only)
				// This way both TX/RX NODE functions are tested on 1 end at the GATEWAY
				if (theStats.ackCount++%3==0) {
					LOG(" Pinging node %d - ACK ", theNodeID);
					//delay(3); //need this when sending right after reception .. ?
					usleep(3000);
					theStats.messageSent++;
					if (rfm69->sendWithRetry(theNodeID, "ACK TEST", 8)) { // 0 = only 1 attempt, no retries
						theStats.ackReceived++;
						LOG("ok!\n");
					}
					else {
						theStats.ackMissed++;
						LOG("nothing\n");
					}
				}
			}//end if radio.ACK_REQESTED
		} //end if radio.receive

		if (sendMQTT == 1) {
			//send var1_usl
			MQTTSendULong(m, sensorNode.nodeID, sensorNode.sensorID, 1, sensorNode.var1_usl);

			//send var2_float
			MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 2, sensorNode.var2_float);

			//send var3_float
			MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 3, sensorNode.var3_float);

			//send var4_int, RSSI
			MQTTSendInt(m, sensorNode.nodeID, sensorNode.sensorID, 4, sensorNode.var4_int);

			sendMQTT = 0;
		}//end if sendMQTT
	}

	mosquitto_destroy(m);
	(void)mosquitto_lib_cleanup();

	if (res == MOSQ_ERR_SUCCESS) {
		return 0;
	} else {
		return 1;
	}
}

static int initRfm(RFM69 *rfm) {
	rfm->restart
(theConfig.frequency,theConfig.nodeId,theConfig.networkId);
	if (theConfig.isRFM69HW)
		rfm->setHighPower(); //uncomment only for RFM69HW!
	if (theConfig.keyLength)
		rfm->encrypt(theConfig.key);
	rfm->promiscuous(theConfig.promiscuousMode);
	LOG("Listening at %d Mhz...\n", theConfig.frequency==RF69_433MHZ ? 433 : theConfig.frequency==RF69_868MHZ ? 868 : 915);
}

/* Fail with an error message. */
static void die(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(1);
}

static long millis(void) {
	struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((tv.tv_sec) * 1000 + tv.tv_usec/1000.0) + 0.5;
	}

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val) {
	char buff_topic[6];
	char buff_message[7];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%04d%", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%u", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
	}

static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	snprintf(buff_message, 12, "%f", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, buff_topic, strlen(buff_message), buff_message, 0, false);

	}

// Handing of Mosquitto messages
void callback(char* topic, uint8_t* payload, unsigned int length) {
	// handle message arrived
	LOG("Mosquitto Callback\n");
}


/* Connect to the network. */
static bool connect(struct mosquitto *m) {
	int res = mosquitto_connect(m, BROKER_HOSTNAME, BROKER_PORT, KEEPALIVE_SECONDS);
	LOG("Connect return %d\n", res);
	return res == MOSQ_ERR_SUCCESS;
}

/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
	if (res == 0) {   /* success */
		LOG("Connect succeed\n");
	} else {
		die("connection refused\n");
	}
}

/* Handle a message that just arrived via one of the subscriptions. */
static void on_message(struct mosquitto *m, void *udata,
const struct mosquitto_message *msg) {
	if (msg == NULL) { return; }
	LOG("-- got message @ %s: (%d, QoS %d, %s) '%s'\n",
		msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
		msg->payload);
	if (strlen((const char *)msg) < strlen(MQTT_ROOT) + 2 + 3 + 1) {return; }	// message is smaller than "RFM/xxx/x" so likey invalid

	Payload data;
	uint8_t network;

	sscanf(msg->topic, "RFM/%d/%d", &network, &data.nodeID);
	if (strncmp(msg->topic, MQTT_ROOT, strlen(MQTT_ROOT)) == 0) {
		
		// extract the target network and the target node from the topic
		sscanf(msg->topic, "RFM/%d/%d", &network, &data.nodeID);
		
		if (network == theConfig.networkId) {
			// only process the messages to our network
		
			sscanf((const char *)msg->payload, "%03d,%ld,%f,%f", &data.sensorID, &data.var1_usl, &data.var2_float, &data.var3_float);
			
			LOG("Received message for Node ID = %d Device ID = %d Time = %d  var2 = %f var3 = %f\n",
				data.nodeID,
				data.sensorID,
				data.var1_usl,
				data.var2_float,
				data.var3_float
			);

			LOG("Message sent to node %d ", data.nodeID);
			theStats.messageSent++;
			if (rfm69->sendWithRetry(data.nodeID,(const void*)(&data),sizeof(data))) {
				LOG("ACK\n");
				theStats.ackReceived++;
				}
			else {
				LOG("NAK\n");
				theStats.ackMissed++;
			}
		}
	}
}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
//	LOG(" -- published successfully\n");
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid,
		int qos_count, const int *granted_qos) {
//	LOG(" -- subscribed successfully\n");
}

/* Register the callbacks that the mosquitto connection will use. */
static bool set_callbacks(struct mosquitto *m) {
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_publish_callback_set(m, on_publish);
	mosquitto_subscribe_callback_set(m, on_subscribe);
	mosquitto_message_callback_set(m, on_message);
	return true;
}

