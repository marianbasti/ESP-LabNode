import streamlit as st
import requests
import pandas as pd
import sqlite3
from datetime import datetime
import time
import plotly.express as px
import json
import threading
from queue import Queue
import asyncio
from collections import defaultdict
import os
from streamlit_cookies_controller import CookieController
import fcntl
import mmap
import select
import struct
import array
import base64
import logging
import traceback

# V4L2 constants
VIDIOC_QUERYCAP = 0x80685600
VIDIOC_ENUM_FMT = 0xc0405602
V4L2_PIX_FMT_MJPEG = 0x47504A4D
V4L2_BUF_TYPE_VIDEO_CAPTURE = 1

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('dashboard.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# Enhanced database setup
def init_db():
    try:
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        c.execute('''CREATE TABLE IF NOT EXISTS devices
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      name TEXT UNIQUE,
                      url TEXT,
                      hostname TEXT,
                      reading_frequency INTEGER DEFAULT 60)''')
        c.execute('''CREATE TABLE IF NOT EXISTS readings
                     (timestamp DATETIME,
                      device_id INTEGER,
                      temperature REAL,
                      humidity REAL,
                      FOREIGN KEY(device_id) REFERENCES devices(id))''')
        c.execute('''CREATE TABLE IF NOT EXISTS video_devices
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      device_path TEXT UNIQUE,
                      device_name TEXT,
                      device_id INTEGER,
                      FOREIGN KEY(device_id) REFERENCES devices(id))''')
        conn.commit()
        logger.info("Database initialized successfully")
    except Exception as e:
        logger.error(f"Database initialization failed: {str(e)}\n{traceback.format_exc()}")
        raise
    finally:
        conn.close()

# Enhanced API interaction functions
def get_sensor_data(base_url):
    try:
        response = requests.get(f"{base_url}/api/sensor")
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to get sensor data from {base_url}: {str(e)}")
        return None
    except ValueError as e:
        logger.error(f"Invalid JSON response from {base_url}: {str(e)}")
        return None

def set_relay_state(base_url, state):
    try:
        response = requests.post(f"{base_url}/api/relay", params={"state": state})
        response.raise_for_status()
        logger.info(f"Relay state set to {state} for {base_url}")
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to set relay state for {base_url}: {str(e)}")
        return None

def get_timer_config(base_url):
    try:
        response = requests.get(f"{base_url}/api/timer")
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to get timer config from {base_url}: {str(e)}")
        return None

def set_timer_config(base_url, config):
    try:
        response = requests.post(f"{base_url}/api/timer", json=config)
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to set timer config for {base_url}: {str(e)}")
        return None

def get_hostname(base_url):
    try:
        response = requests.get(f"{base_url}/api/hostname")
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to get hostname from {base_url}: {str(e)}")
        return None

def set_hostname(base_url, hostname):
    try:
        response = requests.post(f"{base_url}/api/hostname", json={"hostname": hostname})
        response.raise_for_status()
        return response.json()
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to set hostname for {base_url}: {str(e)}")
        return None

# Enhanced device management functions
def add_device(name, url):
    conn = None
    try:
        conn = sqlite3.connect('sensor_data.db')
        c = conn.cursor()
        hostname_info = get_hostname(url)
        hostname = hostname_info.get('hostname', 'unknown') if hostname_info else 'unknown'
        c.execute("INSERT INTO devices (name, url, hostname) VALUES (?, ?, ?)",
                 (name, url, hostname))
        conn.commit()
        logger.info(f"Added new device: {name} ({url})")
        return True
    except sqlite3.IntegrityError as e:
        logger.warning(f"Failed to add device - duplicate name: {name}")
        return False
    except Exception as e:
        logger.error(f"Error adding device {name}: {str(e)}\n{traceback.format_exc()}")
        return False
    finally:
        if conn:
            conn.close()

def remove_device(device_id):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("DELETE FROM devices WHERE id = ?", (device_id,))
        c.execute("DELETE FROM readings WHERE device_id = ?", (device_id,))
        conn.commit()
        logger.info(f"Removed device with ID: {device_id}")
    except Exception as e:
        logger.error(f"Error removing device {device_id}: {str(e)}\n{traceback.format_exc()}")
    finally:
        conn.close()

def get_devices():
    conn = sqlite3.connect('sensor_data.db')
    try:
        df = pd.read_sql_query("SELECT * FROM devices", conn)
        return df
    except Exception as e:
        logger.error(f"Error fetching devices: {str(e)}\n{traceback.format_exc()}")
        return pd.DataFrame()
    finally:
        conn.close()

# Add new functions for reading frequency management
def update_device_frequency(device_id, frequency):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("UPDATE devices SET reading_frequency = ? WHERE id = ?", (frequency, device_id))
        conn.commit()
        logger.info(f"Updated reading frequency for device ID {device_id} to {frequency} seconds")
    except Exception as e:
        logger.error(f"Error updating reading frequency for device ID {device_id}: {str(e)}\n{traceback.format_exc()}")
    finally:
        conn.close()

# Data storage
def store_reading(device_id, temperature, humidity):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("INSERT INTO readings VALUES (?, ?, ?, ?)",
                  (datetime.now(), device_id, temperature, humidity))
        conn.commit()
        logger.info(f"Stored reading for device ID {device_id}: temp={temperature}°C, humidity={humidity}%")
    except Exception as e:
        logger.error(f"Error storing reading for device ID {device_id}: {str(e)}\n{traceback.format_exc()}")
    finally:
        conn.close()

def get_historical_data(device_id, hours=24):
    conn = sqlite3.connect('sensor_data.db')
    try:
        query = f"""
        SELECT timestamp, temperature, humidity 
        FROM readings 
        WHERE device_id = ?
        AND timestamp >= datetime('now', '-{hours} hours')
        """
        df = pd.read_sql_query(query, conn, params=(device_id,), parse_dates=['timestamp'])
        return df
    except Exception as e:
        logger.error(f"Error fetching historical data for device ID {device_id}: {str(e)}\n{traceback.format_exc()}")
        return pd.DataFrame()
    finally:
        conn.close()

def get_available_video_devices():
    video_devices = []
    try:
        for i in range(64):
            try:
                device_path = f'/dev/video{i}'
                with open(device_path, 'rb') as f:
                    cap = array.array('B', [0] * 104)
                    try:
                        fcntl.ioctl(f, VIDIOC_QUERYCAP, cap)
                        device_name = cap[8:40].tobytes().decode().rstrip('\0')
                        video_devices.append({
                            'path': device_path,
                            'name': device_name
                        })
                        logger.debug(f"Found video device: {device_path} ({device_name})")
                    except Exception as e:
                        logger.debug(f"Failed to query video device {device_path}: {str(e)}")
                        continue
            except Exception as e:
                logger.debug(f"Cannot access video device {i}: {str(e)}")
                continue
        return video_devices
    except Exception as e:
        logger.error(f"Error enumerating video devices: {str(e)}\n{traceback.format_exc()}")
        return []

def add_video_device(device_path, device_name, device_id):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("INSERT INTO video_devices (device_path, device_name, device_id) VALUES (?, ?, ?)",
                 (device_path, device_name, device_id))
        conn.commit()
        logger.info(f"Added video device: {device_name} ({device_path}) for device ID {device_id}")
        return True
    except sqlite3.IntegrityError as e:
        logger.warning(f"Failed to add video device - duplicate path: {device_path}")
        return False
    except Exception as e:
        logger.error(f"Error adding video device {device_name} ({device_path}): {str(e)}\n{traceback.format_exc()}")
        return False
    finally:
        conn.close()

def get_device_video_sources(device_id):
    conn = sqlite3.connect('sensor_data.db')
    try:
        df = pd.read_sql_query("SELECT * FROM video_devices WHERE device_id = ?", 
                              conn, params=(device_id,))
        return df
    except Exception as e:
        logger.error(f"Error fetching video sources for device ID {device_id}: {str(e)}\n{traceback.format_exc()}")
        return pd.DataFrame()
    finally:
        conn.close()

def remove_video_device(video_id):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("DELETE FROM video_devices WHERE id = ?", (video_id,))
        conn.commit()
        logger.info(f"Removed video device with ID: {video_id}")
    except Exception as e:
        logger.error(f"Error removing video device {video_id}: {str(e)}\n{traceback.format_exc()}")
    finally:
        conn.close()

# Initialize database and collector outside of main()
init_db()

# Register a cleanup handler
def cleanup():
    pass
import atexit
atexit.register(cleanup)

def check_password():
    """Returns `True` if the user had the correct password."""
    controller = CookieController()
    
    # Check if already authenticated via cookie
    auth_cookie = controller.get('auth_token')
    if auth_cookie == os.environ.get("GUI_PW"):
        return True
    
    def password_entered():
        """Checks whether a password entered by the user is correct."""
        if "password" in st.session_state and st.session_state["password"] == os.environ.get("GUI_PW"):
            st.session_state["password_correct"] = True
            # Set authentication cookie
            controller.set('auth_token', os.environ.get("GUI_PW"))
            del st.session_state["password"]
        else:
            st.session_state["password_correct"] = False

    if "password_correct" not in st.session_state:
        # First run, show input for password
        st.text_input(
            "Password", type="password", on_change=password_entered, key="password"
        )
        return False
    elif not st.session_state["password_correct"]:
        # Password incorrect, show input + error
        st.text_input(
            "Password", type="password", on_change=password_entered, key="password"
        )
        st.error("😕 Password incorrect")
        return False
    else:
        # Password correct
        return True

# Streamlit interface
def main():
    try:
        st.set_page_config(page_title="Temperature Control Dashboard", layout="wide")
        
        if not os.environ.get("GUI_PW"):
            logger.error("Environment variable GUI_PW not set!")
            st.error("Environment variable GUI_PW not set!")
            return
            
        if not check_password():
            st.stop()  # Do not continue if password is incorrect
        
        # Add logout button in sidebar
        if st.sidebar.button("Logout"):
            controller = CookieController()
            controller.remove('auth_token')
            st.rerun()
        
        # Initialize database
        init_db()
        
        # Sidebar device management
        st.sidebar.title("Device Management")
        
        # Add new device
        with st.sidebar.expander("Add New Device"):
            new_name = st.text_input("Device Name")
            new_url = st.text_input("Device URL", "http://")
            if st.button("Add Device"):
                if add_device(new_name, new_url):
                    st.success(f"Added device: {new_name}")
                else:
                    st.error("Failed to add device. Name must be unique.")
        
        # Device selection
        devices = get_devices()
        if len(devices) == 0:
            st.sidebar.warning("No devices configured")
            return
            
        selected_device = st.sidebar.selectbox(
            "Select Device",
            devices.itertuples(),
            format_func=lambda x: x.name
        )
        
        # Remove device button
        if st.sidebar.button("Remove Selected Device"):
            remove_device(selected_device.id)
            st.rerun()
        
        # Main content
        st.title(f"Temperature Control Dashboard - {selected_device.name}")
        
        # Create columns for layout
        col1, col2 = st.columns(2)
        
        # Current readings
        with col1:
            st.subheader("Current Readings")
            data = get_sensor_data(selected_device.url)
            if data and 'error' not in data:
                logger.info(f"Retrieved sensor data for {selected_device.name}: temp={data['temperature']}°C, humidity={data['humidity']}%")
                st.metric("Temperature", f"{data['temperature']}°C")
                st.metric("Humidity", f"{data['humidity']}%")
                store_reading(selected_device.id, data['temperature'], data['humidity'])
            else:
                logger.error(f"Failed to fetch readings for {selected_device.name}")
                st.error("Failed to fetch current readings")
        
        # Manual control
        with col2:
            st.subheader("Manual Control")
            col3, col4 = st.columns(2)
            with col3:
                if st.button("Turn ON"):
                    set_relay_state(selected_device.url, "on")
            with col4:
                if st.button("Turn OFF"):
                    set_relay_state(selected_device.url, "off")
        
        # Timer configuration
        st.subheader("Timer Configuration")
        timer_data = get_timer_config(selected_device.url)
        if timer_data:
            col5, col6, col7 = st.columns(3)
            with col5:
                timer_enabled = st.checkbox("Enable Timer", 
                                          value=timer_data.get('enabled', False))
            with col6:
                on_duration = st.number_input("ON Duration (seconds)", 
                                            value=timer_data.get('onDuration', 0))
            with col7:
                off_duration = st.number_input("OFF Duration (seconds)", 
                                             value=timer_data.get('offDuration', 0))
            
            if st.button("Update Timer"):
                timer_config = {
                    "enabled": timer_enabled,
                    "onDuration": on_duration,
                    "offDuration": off_duration
                }
                result = set_timer_config(selected_device.url, timer_config)
                if result:
                    st.success("Timer updated successfully")
        
        # Add reading frequency control
        with st.sidebar.expander("Device Settings"):
            current_frequency = selected_device.reading_frequency
            new_frequency = st.number_input(
                "Reading Frequency (seconds)",
                min_value=5,
                value=current_frequency,
                step=5
            )
            if new_frequency != current_frequency:
                update_device_frequency(selected_device.id, new_frequency)
                st.success(f"Updated reading frequency to {new_frequency} seconds")

        # Video device management
        with st.sidebar.expander("Video Devices"):
            available_cameras = get_available_video_devices()
            if available_cameras:
                st.write("Available cameras:")
                selected_camera = st.selectbox(
                    "Select camera",
                    options=available_cameras,
                    format_func=lambda x: f"{x['name']} ({x['path']})"
                )
                if st.button("Add Camera"):
                    if add_video_device(selected_camera['path'], 
                                      selected_camera['name'],
                                      selected_device.id):
                        st.success("Camera added successfully")
                    else:
                        st.error("Failed to add camera")
            
            st.write("Connected cameras:")
            video_sources = get_device_video_sources(selected_device.id)
            for _, video in video_sources.iterrows():
                col1, col2 = st.columns([3, 1])
                with col1:
                    st.write(f"{video['device_name']} ({video['device_path']})")
                with col2:
                    if st.button("Remove", key=f"remove_video_{video['id']}"):
                        remove_video_device(video['id'])
                        st.rerun()

        # Historical data visualization
        st.subheader("Historical Data")
        hours = st.slider("Time Window (hours)", 1, 72, 24)
        df = get_historical_data(selected_device.id, hours)
        
        if not df.empty:
            fig1 = px.line(df, x='timestamp', y='temperature', 
                           title='Temperature History')
            st.plotly_chart(fig1, use_container_width=True)
            
            fig2 = px.line(df, x='timestamp', y='humidity', 
                           title='Humidity History')
            st.plotly_chart(fig2, use_container_width=True)
        else:
            st.info("No historical data available")

        # Add video feed display after the timer configuration
        video_sources = get_device_video_sources(selected_device.id)
        if not video_sources.empty:
            st.subheader("Video Feed")
            selected_video = st.selectbox(
                "Select video source",
                video_sources.itertuples(),
                format_func=lambda x: x.device_name
            )
            
            if st.button("Start Stream"):
                try:
                    with open(selected_video.device_path, 'rb') as video:
                        # Create a placeholder for the video frame
                        frame_placeholder = st.empty()
                        
                        # Basic streaming loop
                        while True:
                            try:
                                # Read raw data (this is a simplified version)
                                data = video.read(614400)  # 640x480 YUYV
                                if data:
                                    # Convert to base64 for display
                                    b64_frame = base64.b64encode(data).decode()
                                    frame_placeholder.image(
                                        f"data:image/jpeg;base64,{b64_frame}",
                                        use_column_width=True
                                    )
                                time.sleep(0.1)  # Limit frame rate
                            except Exception as e:
                                st.error(f"Streaming error: {str(e)}")
                                break
                except Exception as e:
                    st.error(f"Failed to open video device: {str(e)}")
    except Exception as e:
        logger.error(f"Unhandled exception in main: {str(e)}\n{traceback.format_exc()}")
        st.error("An unexpected error occurred. Please check the logs for details.")

if __name__ == "__main__":
    main()
