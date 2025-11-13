#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <math.h>

typedef struct {
    float battery;
    float speed;
    float temp;
} SensorData;

typedef struct {
    float total_distance;
    float top_speed;
    float last_battery;
    bool is_moving;
    SensorData current_sensor;
} VehicleState;

typedef struct {
    SensorData sensor;
    float distance;
    float top_speed;
    long timestamp;
} CloudData;

#define QUEUE_SIZE 1000

typedef struct {
    CloudData data[QUEUE_SIZE];
    int front;
    int rear;
    int count;
    pthread_mutex_t mutex;
} CloudQueue;

VehicleState vehicle_state = {0};
CloudQueue cloud_queue = {0};
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

SensorData read_sensor_data() {
    SensorData data;
    
    static float battery = 100.0;
    battery -= 0.001;
    if (battery < 0) battery = 100.0;
    data.battery = battery;
    
    static float speed = 0.0;
    speed += ((float)rand() / RAND_MAX - 0.5) * 5.0;
    if (speed < 0) speed = 0;
    if (speed > 80) speed = 80;
    data.speed = speed;
    
    static float temp = 25.0;
    temp += ((float)rand() / RAND_MAX - 0.5) * 2.0;
    if (temp < 20) temp = 20;
    if (temp > 75) temp = 75;
    data.temp = temp;
    
    return data;
}

typedef enum { SUCCESS, FAILURE } CloudStatus;

CloudStatus send_to_cloud(CloudData* data) {
    int delay = 1 + rand() % 10;
    sleep(delay);
    
    if (rand() % 10 < 9) {
        printf("[CLOUD] Sent: Battery=%.1f%%, Speed=%.1f km/h, Temp=%.1f°C, Dist=%.2f km\n",
               data->sensor.battery, data->sensor.speed, data->sensor.temp, data->distance);
        return SUCCESS;
    } else {
        printf("[CLOUD] Send failed\n");
        return FAILURE;
    }
}

void queue_init(CloudQueue* q) {
    q->front = 0;
    q->rear = -1;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

bool queue_enqueue(CloudQueue* q, CloudData* data) {
    pthread_mutex_lock(&q->mutex);
    
    if (q->count >= QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    
    q->rear = (q->rear + 1) % QUEUE_SIZE;
    q->data[q->rear] = *data;
    q->count++;
    
    pthread_mutex_unlock(&q->mutex);
    return true;
}

bool queue_dequeue(CloudQueue* q, CloudData* data) {
    pthread_mutex_lock(&q->mutex);
    
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    
    *data = q->data[q->front];
    q->front = (q->front + 1) % QUEUE_SIZE;
    q->count--;
    
    pthread_mutex_unlock(&q->mutex);
    return true;
}

long get_timestamp_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void* fast_loop_thread(void* arg) {
    printf("[Sensor Thread] Started\n");
    
    while (1) {
        SensorData sensor = read_sensor_data();
        
        pthread_mutex_lock(&state_mutex);
        
        vehicle_state.current_sensor = sensor;
        
        // Calculate distance: speed * time (10ms = 0.01/3600 hours)
        float time_hours = 0.01 / 3600.0;
        vehicle_state.total_distance += sensor.speed * time_hours;
        
        if (sensor.speed > vehicle_state.top_speed) {
            vehicle_state.top_speed = sensor.speed;
        }
        
        vehicle_state.is_moving = (sensor.speed > 0.5);
        
        pthread_mutex_unlock(&state_mutex);
        
        usleep(10000);  // 10ms for 100Hz
    }
    
    return NULL;
}

void* slow_loop_thread(void* arg) {
    printf("[Logic Thread] Started\n");
    
    long last_send_time = get_timestamp_ms();
    float last_battery_sent = 100.0;
    
    while (1) {
        pthread_mutex_lock(&state_mutex);
        
        SensorData current = vehicle_state.current_sensor;
        bool is_moving = vehicle_state.is_moving;
        float battery_change = fabs(current.battery - last_battery_sent);
        bool temp_critical = current.temp > 70.0;
        
        CloudData cloud_data;
        cloud_data.sensor = current;
        cloud_data.distance = vehicle_state.total_distance;
        cloud_data.top_speed = vehicle_state.top_speed;
        cloud_data.timestamp = get_timestamp_ms();
        
        bool should_send = false;
        
        // Rule 1: Battery change when idle
        if (!is_moving && battery_change > 0.5) {
            printf("[Logic] Battery changed while idle\n");
            should_send = true;
            last_battery_sent = current.battery;
        }
        
        // Rule 2: Send every second when moving
        if (is_moving && (cloud_data.timestamp - last_send_time >= 1000)) {
            printf("[Logic] Periodic update (moving)\n");
            should_send = true;
            last_send_time = cloud_data.timestamp;
        }
        
        // Rule 3: Critical temperature
        if (temp_critical) {
            printf("[Logic] CRITICAL TEMP ALERT!\n");
            should_send = true;
        }
        
        pthread_mutex_unlock(&state_mutex);
        
        if (should_send) {
            if (!queue_enqueue(&cloud_queue, &cloud_data)) {
                printf("[Logic] Warning: Queue full!\n");
            }
        }
        
        usleep(100000);
    }
    
    return NULL;
}

void* cloud_sender_thread(void* arg) {
    printf("[Cloud Thread] Started\n");
    
    while (1) {
        CloudData data;
        
        if (queue_dequeue(&cloud_queue, &data)) {
            CloudStatus status = send_to_cloud(&data);
            
            if (status == FAILURE) {
                printf("[Cloud] Retrying...\n");
                queue_enqueue(&cloud_queue, &data);
            }
        } else {
            usleep(100000);
        }
    }
    
    return NULL;
}

int main() {
    printf("=== Vehicle Sensor Monitoring System ===\n\n");
    
    srand(time(NULL));
    
    queue_init(&cloud_queue);
    
    pthread_t fast_thread, slow_thread, cloud_thread;
    
    pthread_create(&fast_thread, NULL, fast_loop_thread, NULL);
    pthread_create(&slow_thread, NULL, slow_loop_thread, NULL);
    pthread_create(&cloud_thread, NULL, cloud_sender_thread, NULL);
    
    pthread_join(fast_thread, NULL);
    pthread_join(slow_thread, NULL);
    pthread_join(cloud_thread, NULL);
    
    return 0;
}