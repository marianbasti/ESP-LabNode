import sqlite3
import requests
import time
from datetime import datetime
import signal
import sys
import logging
from threading import Event, Thread
from collections import defaultdict

logging.basicConfig(level=logging.INFO,
                   format='%(asctime)s - %(levelname)s - %(message)s')

class DeviceCollector:
    def __init__(self):
        self.collectors = {}
        self.stop_flags = defaultdict(Event)
        self.main_stop_flag = Event()
        self.last_activation = defaultdict(int)
        self.timer_states = defaultdict(lambda: {"last_switch": 0, "current_state": "off"})
        
    def get_sensor_data(self, base_url):
        try:
            response = requests.get(f"{base_url}/api/sensor")
            return response.json()
        except:
            return None

    def set_relay_state(self, base_url, state):
        try:
            response = requests.post(f"{base_url}/api/relay", params={"state": state})
            return response.json()
        except:
            return None

    def store_reading(self, device_id, temperature, humidity):
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        c.execute("INSERT INTO readings VALUES (?, ?, ?, ?)",
                  (datetime.now(), device_id, temperature, humidity))
        conn.commit()
        conn.close()

    def get_timer_config(self, base_url):
        try:
            response = requests.get(f"{base_url}/api/timer")
            return response.json()
        except:
            return None

    def collect_device_data(self, device_id, device_url, frequency):
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        
        while not self.stop_flags[device_id].is_set() and not self.main_stop_flag.is_set():
            try:
                # Get device settings
                c.execute("""SELECT humidity_control, humidity_threshold, 
                           humidity_on_time, humidity_cooldown 
                           FROM devices WHERE id = ?""", (device_id,))
                settings = c.fetchone()
                
                # Get timer configuration
                timer_config = self.get_timer_config(device_url)
                current_time = time.time()
                
                # Handle timer control
                if timer_config and timer_config.get('enabled'):
                    timer_state = self.timer_states[device_id]
                    on_duration = timer_config.get('onDuration', 0)
                    off_duration = timer_config.get('offDuration', 0)
                    
                    if timer_state["current_state"] == "off" and current_time - timer_state["last_switch"] >= off_duration:
                        self.set_relay_state(device_url, "on")
                        timer_state["current_state"] = "on"
                        timer_state["last_switch"] = current_time
                        logging.info(f"Timer turned ON device {device_id}")
                    elif timer_state["current_state"] == "on" and current_time - timer_state["last_switch"] >= on_duration:
                        self.set_relay_state(device_url, "off")
                        timer_state["current_state"] = "off"
                        timer_state["last_switch"] = current_time
                        logging.info(f"Timer turned OFF device {device_id}")
                
                # Handle humidity control only if timer is not enabled
                elif settings and settings[0] and not (timer_config and timer_config.get('enabled')):
                    humidity_control, threshold, on_time, cooldown = settings
                    data = self.get_sensor_data(device_url)
                    
                    if data and 'error' not in data:
                        self.store_reading(device_id, data['temperature'], data['humidity'])
                        
                        if (data['humidity'] < threshold and 
                            current_time - self.last_activation[device_id] >= cooldown):
                            self.set_relay_state(device_url, "on")
                            logging.info(f"Turned ON device {device_id} due to humidity {data['humidity']} below {threshold}")
                            time.sleep(on_time)
                            self.set_relay_state(device_url, "off")
                            self.last_activation[device_id] = current_time
                            logging.info(f"Turned OFF device {device_id} after {on_time} seconds")
                
                # Get sensor data even if no control is active
                if not (timer_config and timer_config.get('enabled')):
                    data = self.get_sensor_data(device_url)
                    if data and 'error' not in data:
                        self.store_reading(device_id, data['temperature'], data['humidity'])
                
            except Exception as e:
                logging.error(f"Error in device control loop for device {device_id}: {e}")
            
            time.sleep(frequency)
        
        conn.close()

    def start_collector(self, device_id, device_url, frequency):
        if device_id in self.collectors:
            self.stop_flags[device_id].set()
            self.collectors[device_id].join()
        
        self.stop_flags[device_id].clear()
        collector = Thread(
            target=self.collect_device_data,
            args=(device_id, device_url, frequency)
        )
        collector.daemon = True
        collector.start()
        self.collectors[device_id] = collector

    def get_active_devices(self):
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        c.execute("SELECT id, url, reading_frequency FROM devices")
        devices = c.fetchall()
        conn.close()
        return devices

    def run(self):
        def signal_handler(signum, frame):
            logging.info("Stopping collector service...")
            self.main_stop_flag.set()
            for device_id in list(self.collectors.keys()):
                self.stop_flags[device_id].set()
            sys.exit(0)

        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)

        while not self.main_stop_flag.is_set():
            devices = self.get_active_devices()
            for device_id, url, frequency in devices:
                if device_id not in self.collectors:
                    logging.info(f"Starting collector for device {device_id}")
                    self.start_collector(device_id, url, frequency)
            time.sleep(10)  # Check for new devices every 10 seconds

if __name__ == "__main__":
    collector = DeviceCollector()
    collector.run()
