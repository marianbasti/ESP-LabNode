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
import asyncio, aiohttp
from collections import defaultdict
import os
from streamlit_cookies_controller import CookieController
import logging
import traceback

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
        # Add new columns for humidity control
        c.execute('''ALTER TABLE devices ADD COLUMN humidity_control INTEGER DEFAULT 0''')
        c.execute('''ALTER TABLE devices ADD COLUMN humidity_threshold REAL DEFAULT 0.0''')
        c.execute('''ALTER TABLE devices ADD COLUMN humidity_on_time INTEGER DEFAULT 300''')
        c.execute('''ALTER TABLE devices ADD COLUMN humidity_cooldown INTEGER DEFAULT 600''')
        conn.commit()
        logger.info("Database initialized successfully")
    except sqlite3.OperationalError as e:
        if 'duplicate column' not in str(e):
            raise
    except Exception as e:
        logger.error(f"Database initialization failed: {str(e)}\n{traceback.format_exc()}")
        raise
    finally:
        conn.close()

# Enhanced API interaction functions
def get_sensor_data(base_url):
    try:
        headers = {'Connection': 'close'}
        response = requests.get(
            f"{base_url}/api/sensor",
            headers=headers,
            timeout=5.0
        )
        response.raise_for_status()
        return response.json()
    except requests.exceptions.Timeout:
        logger.error(f"Timeout getting sensor data from {base_url}")
        return None
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to get sensor data from {base_url}: {str(e)}")
        return None
    except ValueError as e:
        logger.error(f"Invalid JSON response from {base_url}: {str(e)}")
        return None

def set_relay_state(base_url, state):
    try:
        headers = {
            'Content-Type': 'application/json',
        }
        data = f'{{"state":"{state}"}}'  # Format JSON string properly
        
        response = requests.post(
            f"{base_url}/api/relay",
            headers=headers,
            data=data,  # Use data instead of params
            timeout=5.0
        )
        response.raise_for_status()
        logger.info(f"Relay state set to {state} for {base_url}")
        return response.json()
    except requests.exceptions.Timeout:
        logger.error(f"Timeout setting relay state for {base_url}")
        return None
    except requests.exceptions.RequestException as e:
        logger.error(f"Failed to set relay state for {base_url}: {str(e)}")
        return None
    except ValueError as e:
        logger.error(f"Invalid JSON response from {base_url}: {str(e)}")
        return None

async def set_relay_state_async(base_url, state):
    try:
        headers = {
            'Content-Type': 'application/json',
        }
        data = f'{{"state":"{state}"}}'
        
        async with aiohttp.ClientSession() as session:
            async with session.post(
                f"{base_url}/api/relay",
                headers=headers,
                data=data,
                timeout=2.0
            ) as response:
                response.raise_for_status()
                result = await response.json()
                logger.info(f"Relay state set to {state} for {base_url}")
                return result
    except Exception as e:
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

# Add new functions for humidity control settings
def update_humidity_settings(device_id, enabled, threshold, on_time, cooldown):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        c.execute("""UPDATE devices 
                     SET humidity_control = ?, 
                         humidity_threshold = ?,
                         humidity_on_time = ?,
                         humidity_cooldown = ?
                     WHERE id = ?""", 
                 (enabled, threshold, on_time, cooldown, device_id))
        conn.commit()
        logger.info(f"Updated humidity settings for device ID {device_id}")
    except Exception as e:
        logger.error(f"Error updating humidity settings: {str(e)}")
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

def get_available_time_range(device_id):
    conn = sqlite3.connect('sensor_data.db')
    try:
        query = """
        SELECT 
            MIN(timestamp) as first_reading,
            MAX(timestamp) as last_reading
        FROM readings 
        WHERE device_id = ?
        """
        df = pd.read_sql_query(query, conn, params=(device_id,), parse_dates=['first_reading', 'last_reading'])
        if df.empty or pd.isnull(df['first_reading'].iloc[0]):
            return None, None
        return df['first_reading'].iloc[0], df['last_reading'].iloc[0]
    except Exception as e:
        logger.error(f"Error fetching time range for device ID {device_id}: {str(e)}\n{traceback.format_exc()}")
        return None, None
    finally:
        conn.close()

def show_dashboard_page(selected_device):
    # Create a container for better spacing
    main_container = st.container()
    
    with main_container:
        # Header with device status
        col1, col2 = st.columns([3, 1])
        with col1:
            st.title(f"📊 {selected_device.name}")
        with col2:
            st.markdown(
                f"""
                <div style='background-color: #f0f2f6; padding: 1rem; border-radius: 0.5rem; text-align: center;'>
                    <small>Device Status</small><br/>
                    <span style='color: #00c853; font-size: 1.2em;'>●</span> Online
                </div>
                """,
                unsafe_allow_html=True
            )

        # Current readings in modern cards
        st.markdown("### 📌 Current Status")
        metric_cols = st.columns(2)
        
        with st.spinner('Fetching current readings...'):
            data = get_sensor_data(selected_device.url)
            
        if data and 'error' not in data:
            with metric_cols[0]:
                st.markdown(
                    f"""
                    <div style='background-color: #f0f2f6; padding: 1.5rem; border-radius: 0.5rem;'>
                        <h3 style='margin:0'>🌡️ Temperature</h3>
                        <h2 style='color: #1e88e5; margin:0'>{data['temperature']}°C</h2>
                    </div>
                    """,
                    unsafe_allow_html=True
                )
            with metric_cols[1]:
                st.markdown(
                    f"""
                    <div style='background-color: #f0f2f6; padding: 1.5rem; border-radius: 0.5rem;'>
                        <h3 style='margin:0'>💧 Humidity</h3>
                        <h2 style='color: #1e88e5; margin:0'>{data['humidity']}%</h2>
                    </div>
                    """,
                    unsafe_allow_html=True
                )
            store_reading(selected_device.id, data['temperature'], data['humidity'])
        else:
            st.error("📡 Unable to fetch current readings. Please check device connection.")

        # Control panel
        st.markdown("### 🎮 Control Panel")
        control_cols = st.columns([2, 3])
        
        with control_cols[0]:
            st.markdown("#### Manual Control")
            control_cols_inner = st.columns(2)
            
            # Add session state for button loading states
            if 'button_loading' not in st.session_state:
                st.session_state.button_loading = False
            
            with control_cols_inner[0]:
                if st.button("🟢 Turn ON", 
                            use_container_width=True, 
                            disabled=st.session_state.button_loading):
                    placeholder = st.empty()
                    placeholder.info("Turning ON...")
                    st.session_state.button_loading = True
                    result = set_relay_state(selected_device.url, "on")
                    if result:
                        placeholder.success("Device turned ON")
                    else:
                        placeholder.error("Failed to turn device ON")
                    st.session_state.button_loading = False
                    
            with control_cols_inner[1]:
                if st.button("🔴 Turn OFF", 
                            use_container_width=True,
                            disabled=st.session_state.button_loading):
                    placeholder = st.empty()
                    placeholder.info("Turning OFF...")
                    st.session_state.button_loading = True
                    result = set_relay_state(selected_device.url, "off")
                    if result:
                        placeholder.success("Device turned OFF")
                    else:
                        placeholder.error("Failed to turn device OFF")
                    st.session_state.button_loading = False

        with control_cols[1]:
            st.markdown("#### Timer Settings")
            timer_data = get_timer_config(selected_device.url)
            if timer_data:
                timer_cols = st.columns([1, 2, 2])
                with timer_cols[0]:
                    timer_enabled = st.toggle("Active", value=timer_data.get('enabled', False))
                with timer_cols[1]:
                    on_duration = st.number_input("ON (seconds)", 
                                                value=timer_data.get('onDuration', 300),
                                                min_value=0,
                                                max_value=86400,  # 24 hours
                                                step=5)
                with timer_cols[2]:
                    off_duration = st.number_input("OFF (seconds)", 
                                                 value=timer_data.get('offDuration', 300),
                                                 min_value=0,
                                                 max_value=86400,  # 24 hours
                                                 step=5)

        # Add after the Timer Settings section
        with control_cols[1]:
            st.markdown("#### Humidity Control")
            humidity_control = st.toggle("Enable Humidity Control", 
                                        value=bool(selected_device.humidity_control))
            
            humidity_cols = st.columns([2, 2, 2])
            with humidity_cols[0]:
                threshold = st.number_input("Humidity Threshold (%)",
                                        value=float(selected_device.humidity_threshold or 50.0),
                                        min_value=0.0,
                                        max_value=100.0,
                                        step=1.0)
            with humidity_cols[1]:
                on_time = st.number_input("On Duration (seconds)",
                                        value=int(selected_device.humidity_on_time or 300),
                                        min_value=0,
                                        max_value=3600,  # 1 hour
                                        step=60)
            with humidity_cols[2]:
                cooldown = st.number_input("Minimum Interval (seconds)",
                                        value=int(selected_device.humidity_cooldown or 600),
                                        min_value=0,
                                        max_value=7200,  # 2 hours
                                        step=60)

            if st.button("💾 Save Humidity Settings", use_container_width=True):
                with st.spinner('Updating humidity settings...'):
                    update_humidity_settings(selected_device.id, 
                                        int(humidity_control),
                                        threshold,
                                        on_time,
                                        cooldown)
                    st.success("Humidity settings updated successfully")

        # Historical data with improved visuals
        st.markdown("### 📈 Historical Data")
        
        # Get available time range
        first_reading, last_reading = get_available_time_range(selected_device.id)
        
        if first_reading is None:
            st.info("No historical data available for this device")
        else:
            # Calculate the total hours of available data
            total_hours = (last_reading - first_reading).total_seconds() / 3600
            
            # Create dynamic time options based on available data
            time_options = []
            for hours in [6, 12, 24, 48, 72]:
                if hours <= total_hours:
                    time_options.append(f"Last {hours} hours")
            time_options.append("All data")
            
            if not time_options:
                time_options = ["All data"]
            
            selected_time = st.select_slider(
                "Time Range",
                options=time_options,
                value=time_options[-1]
            )
            
            with st.spinner('Loading historical data...'):
                # Get the number of hours for the query
                if selected_time == "All data":
                    hours = int(total_hours) + 1
                else:
                    hours = int(selected_time.split()[1])
                    
                df = get_historical_data(selected_device.id, hours)
                
                if not df.empty:
                    # Temperature chart
                    fig1 = px.line(df, x='timestamp', y='temperature',
                                title='Temperature History')
                    fig1.update_traces(line_color='#1e88e5')
                    fig1.update_layout(
                        plot_bgcolor='white',
                        paper_bgcolor='white',
                        xaxis_gridcolor='#f0f2f6',
                        yaxis_gridcolor='#f0f2f6'
                    )
                    st.plotly_chart(fig1, use_container_width=True)
                    
                    # Humidity chart
                    fig2 = px.line(df, x='timestamp', y='humidity',
                                title='Humidity History')
                    fig2.update_traces(line_color='#43a047')
                    fig2.update_layout(
                        plot_bgcolor='white',
                        paper_bgcolor='white',
                        xaxis_gridcolor='#f0f2f6',
                        yaxis_gridcolor='#f0f2f6'
                    )
                    st.plotly_chart(fig2, use_container_width=True)
                else:
                    st.info("📊 No data available for the selected time range")

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
            st.stop()

        # Add logout button in sidebar
        if st.sidebar.button("Logout"):
            controller = CookieController()
            controller.remove('auth_token')
            st.rerun()

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

        # Device settings
        with st.sidebar.expander("Device Settings"):
            current_frequency = selected_device.reading_frequency
            new_frequency = st.number_input(
                "Reading Frequency (seconds)",
                min_value=5,
                max_value=3600,  # 1 hour
                value=current_frequency or 60,
                step=5
            )

        # Show dashboard content
        show_dashboard_page(selected_device)

    except Exception as e:
        logger.error(f"Unhandled exception in main: {str(e)}\n{traceback.format_exc()}")
        st.error("An unexpected error occurred. Please check the logs for details.")

if __name__ == "__main__":
    main()
