
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>

#include <mosquitto.h>
#include <pthread.h>

// Defaults
#define DEFAULT_INTERFACE_NAME              "gtp0"
#define PRINT_TO_STDOUT                     1

// MQTT
#define MQTT_TOPIC                          "net_monitor"
#define MQTT_BROKER_KEEPALIVE               40      // Seconds

#define MAX_PAYLOAD_LENGTH                  1024
#define MAX_INTERFACE_STATISTICS_PATH       512
#define MAX_UINT64_LENGTH                   24  // 20
#define MAX_THREADS                         32

_Atomic(int64_t) tx_bytes_1s = 0, rx_bytes_1s = 0, tx_packets_1s = 0, rx_packets_1s = 0;
int interrupted = 0;
int64_t tx_bytes_total = 0, rx_bytes_total = 0, tx_packets_total = 0, rx_packets_total = 0;

static bool read_sysfs_uint64(const char *path, int64_t *value)
{
	char buffer[MAX_UINT64_LENGTH];
	int input_fd = open(path, O_RDONLY);
	if (input_fd == -1) {
		fprintf(stderr, "Error: Unable to open file '%s'.\n", path);
		return false;
	}

	ssize_t read_len = read(input_fd, buffer, sizeof(buffer));
	if (read_len < 0)
	{
		fprintf(stderr, "Error: Unable to read from file '%s'\n", path);
		close(input_fd);
		return false;
	}
	buffer[read_len] = '\0';

	*value = strtoll(buffer, NULL, 10);

	close(input_fd);
	return !(*value == LONG_MAX && errno == ERANGE);
}

/* High-precision sleep with interrupt handling */
static void sleep_f(int seconds)
{
	struct timespec req = { seconds, 0 }, rem = { 0, 0 };
	int result;

	do {
		result = nanosleep(&req, &rem);
		req = rem;
	} while (result == -1 && errno == EINTR);
}

static void *monitoring_thread(void *p)
{
	const unsigned char *interface_name = (const unsigned char *) p;

	int64_t tx_bytes, tx_bytes_last, tx_packets, tx_packets_last;
	int64_t rx_bytes, rx_bytes_last, rx_packets, rx_packets_last;

	char tx_bytes_path[MAX_INTERFACE_STATISTICS_PATH], rx_bytes_path[MAX_INTERFACE_STATISTICS_PATH],
	tx_packets_path[MAX_INTERFACE_STATISTICS_PATH], rx_packets_path[MAX_INTERFACE_STATISTICS_PATH];

	snprintf(tx_bytes_path, sizeof(tx_bytes_path), "/sys/class/net/%s/statistics/tx_bytes", interface_name);
	snprintf(rx_bytes_path, sizeof(rx_bytes_path), "/sys/class/net/%s/statistics/rx_bytes", interface_name);
	snprintf(tx_packets_path, sizeof(tx_packets_path), "/sys/class/net/%s/statistics/tx_packets", interface_name);
	snprintf(rx_packets_path, sizeof(rx_packets_path), "/sys/class/net/%s/statistics/rx_packets", interface_name);

	if (access(tx_bytes_path, F_OK) == -1 || access(rx_bytes_path, F_OK) == -1 ||
		access(tx_packets_path, F_OK) == -1 || access(rx_packets_path, F_OK) == -1)
	{
		fprintf(stderr, "Error: Monitoring error on interface '%s'.\n", interface_name);
		pthread_exit(NULL);
	}

	fprintf(stdout, "Status: Monitoring interface '%s' started...\n", interface_name);
	
	if (!read_sysfs_uint64(tx_bytes_path, &tx_bytes_total) || !read_sysfs_uint64(rx_bytes_path, &rx_bytes_total) ||
		!read_sysfs_uint64(tx_packets_path, &tx_packets_total) || !read_sysfs_uint64(rx_packets_path, &rx_packets_total))	
	{
		fprintf(stderr, "Error: Failed to read values from sysfs (0)\n");
		pthread_exit(NULL);
	}

	while (!interrupted)
	{
		if (!read_sysfs_uint64(tx_bytes_path, &tx_bytes_last) || !read_sysfs_uint64(rx_bytes_path, &rx_bytes_last) ||
			!read_sysfs_uint64(tx_packets_path, &tx_packets_last) || !read_sysfs_uint64(rx_packets_path, &rx_packets_last))
		{
			fprintf(stderr, "Error: Failed to read values from sysfs (1)\n");
			break;
		}

		sleep_f(1); // Wait for 1 second

		if (!read_sysfs_uint64(tx_bytes_path, &tx_bytes) || !read_sysfs_uint64(rx_bytes_path, &rx_bytes) ||
			!read_sysfs_uint64(tx_packets_path, &tx_packets) || !read_sysfs_uint64(rx_packets_path, &rx_packets))
		{
			fprintf(stderr, "Error: Failed to read values from sysfs (2)\n");
			break;
		}

		tx_bytes -= tx_bytes_last;
		rx_bytes -= rx_bytes_last;

		tx_packets -= tx_packets_last;
		rx_packets -= rx_packets_last;

		atomic_store(&tx_bytes_1s, tx_bytes);
		atomic_store(&tx_packets_1s, tx_packets);
		
		atomic_store(&rx_bytes_1s, rx_bytes);
		atomic_store(&rx_packets_1s, rx_packets);
	}

	if (!read_sysfs_uint64(tx_bytes_path, &tx_bytes_last) || !read_sysfs_uint64(rx_bytes_path, &rx_bytes_last) ||
		!read_sysfs_uint64(tx_packets_path, &tx_packets_last) || !read_sysfs_uint64(rx_packets_path, &rx_packets_last))
	{
		fprintf(stderr, "Error: Failed to read values from sysfs (2)\n");
		pthread_exit(NULL);
	}

	tx_bytes_total = tx_bytes_last - tx_bytes_total;
	tx_packets_total = tx_packets_last - tx_packets_total;
	rx_bytes_total = rx_bytes_last - rx_bytes_total;
	rx_packets_total = rx_packets_last - rx_packets_total;

	pthread_exit(NULL);
}

static void monitor_interface(const char *interface_name, const char *broker_address, int broker_port, const char *broker_topic, int sampling_delay)
{
	const bool broker_available = (broker_address && broker_port > 0 && broker_port < 65535 && broker_topic);

	int mqtt_message_id;
	struct mosquitto *mqtt_client = NULL;
	
	if (broker_available)
	{
		mqtt_client = mosquitto_new(NULL, true, NULL);
		if (mqtt_client == NULL)
		{
			fprintf(stderr, "Error: Failed to create a new MQTT client.\n");
			return;
		}

		fprintf(stdout, "Status: Connecting to broker at %s:%d...\n", broker_address, broker_port);

		if (mosquitto_connect(mqtt_client, broker_address, broker_port, MQTT_BROKER_KEEPALIVE) == MOSQ_ERR_SUCCESS)
			fprintf(stdout, "Status: Connected to broker.\n");
		else
		{
			fprintf(stderr, "Error: Failed to connect to the broker.\n");
			mosquitto_destroy(mqtt_client);
			return;
		}
	}

	char payload[MAX_PAYLOAD_LENGTH];
	int payload_len;

	pthread_t thread;
	if (pthread_create(&thread, NULL, monitoring_thread, (void *) interface_name) != 0)
	{
		fprintf(stderr, "Error: Failed to create monitoring thread.\n");
		mosquitto_disconnect(mqtt_client);
		mosquitto_destroy(mqtt_client);
		return;
	}

	while (!interrupted)
	{
		int64_t rx_bytes = atomic_load(&rx_bytes_1s);
		int64_t tx_bytes = atomic_load(&tx_bytes_1s);
		int64_t rx_packets = atomic_load(&rx_packets_1s);
		int64_t tx_packets = atomic_load(&tx_packets_1s);
		
		payload_len = snprintf(payload, sizeof(payload), "{\n"\
														"\"InboundBytes\": \"%.4f MB/s\",\n" \
														"\"OutboundBytes\": \"%.4f MB/s\",\n" \
														"\"InboundPackets\": \"%.4f KP/s\",\n" \
														"\"OutboundPackets\": \"%.4f KP/s\"\n}",
				(double) rx_bytes / (double) (1024 * 1024), (double) tx_bytes / (double) (1024 * 1024),
				(double) rx_packets / (double) (1000), (double) tx_packets / (double) (1000));

		if (broker_available)
		{
			if (mosquitto_publish(mqtt_client, &mqtt_message_id, broker_topic, payload_len, payload, /* qos */ 0, /* retain? */ false) != MOSQ_ERR_SUCCESS)
			{
				fprintf(stderr, "Error: Failed to publish message to the broker on topic '%s'.\n", broker_topic);
				break;
			}

			fprintf(stdout, "Status: Published message %d.\n", mqtt_message_id);
		}

		#if PRINT_TO_STDOUT
		fprintf(stdout, "Data: %.*s\n\n", payload_len, payload);
		#endif

		sleep_f(sampling_delay);
	}

	if (broker_available)
	{
		mosquitto_disconnect(mqtt_client);
		mosquitto_destroy(mqtt_client);
	}
	
	pthread_join(thread, NULL);
	return;
}

static void print_help(const char *argv0)
{
	printf("\n");
	printf(" Usage: %s <options>\n", argv0);
	printf("  -i\tInterface name (Default: '%s')\n", DEFAULT_INTERFACE_NAME);
	printf("  -b\tMQTT Broker address\n");
	printf("  -p\tMQTT Broker port\n");
	printf("  -t\tMQTT Topic for the publisher\n");
	printf("  -s\tSampling delay in seconds (Default: 1 second)\n");
	printf("\n");
	printf(" MQTT Broker is optional\n");
	printf("\n");
}

static void interrupt_set(int sig)
{
	(void) sig;
	interrupted = 1;
}

int main(int argc, char *argv[])
{
	signal(SIGINT, interrupt_set); 

	opterr = 0;
	int c;
	char *interface = NULL;
	char *broker_address = NULL;
	char *broker_topic = NULL;
	int broker_port = 0;
	int sampling_delay = 1;	// Second(s)

	while ((c = getopt(argc, argv, "i:b:p:t:s:h")) != -1)
	{
		switch (c)
		{
			case 'i':
				interface = strdup(optarg);
				break;
			
			case 'b':
				broker_address = strdup(optarg);
				break;

			case 'p':
				broker_port = atoi(optarg);
				break;

			case 't':
				broker_topic = strdup(optarg);
				break;

			case 's':
				sampling_delay = atoi(optarg);
				break;

			case 'h':
				print_help(argv[0]);
				return 0;

			case '?':
				if (optopt == 'i' || optopt == 'b' || optopt == 'p' || optopt == 't' || optopt == 's')
					fprintf (stderr, "Error: Option -%c requires an argument.\n", optopt);
				else if (isprint(optopt))
					fprintf (stderr, "Error: Unknown option '-%c'.\n", optopt);
				else
					fprintf (stderr, "Error: Unknown option character '\\x%x'.\n", optopt);
				return -1;

			default:
				print_help(argv[0]);
				return -1;
		}
	}

	mosquitto_lib_init();
	
	if (interface == NULL)
	{
		fprintf(stdout, "Note: No interface supplied, using default interface '%s'.\n", DEFAULT_INTERFACE_NAME);
		interface = strdup(DEFAULT_INTERFACE_NAME);
	}

	if (broker_address == NULL || broker_port < 0 || broker_port > 65535)
		fprintf(stderr, "Warning: MQTT publishing is not available, invalid or no broker address/port supplied.\n");
	
	monitor_interface(interface, broker_address, broker_port, broker_topic, sampling_delay);
	
	if (interface) free(interface);
	if (broker_address) free(broker_address);
	if (broker_topic) free(broker_topic);

	mosquitto_lib_cleanup();

	fprintf(stdout, 
				"\n"\
				" ------ Statistics ------\n" \
				" Total inbound bytes: %.4f MB\n" \
				" Total outbound bytes: %.4f MB\n" \
				" Total inbound packets: %.4f KP\n" \
				" Total outbound packets: %.4f KP\n" \
				"\n",
				(double) rx_bytes_total / (double) (1024 * 1024), (double) tx_bytes_total / (double) (1024 * 1024),
				(double) rx_packets_total / (double) (1000), (double) tx_packets_total / (double) (1000));
	return 0;
}
