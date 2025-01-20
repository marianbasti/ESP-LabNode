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
        
    def get_sensor_data(self, base_url):
        try:
            response = requests.get(f"{base_url}/api/sensor")
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

    def collect_device_data(self, device_id, device_url, frequency):
        while not self.stop_flags[device_id].is_set() and not self.main_stop_flag.is_set():
            try:
                data = self.get_sensor_data(device_url)
                if data and 'error' not in data:
                    self.store_reading(device_id, data['temperature'], data['humidity'])
                    logging.info(f"Collected data for device {device_id}: {data}")
            except Exception as e:
                logging.error(f"Error collecting data for device {device_id}: {e}")
            time.sleep(frequency)

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
