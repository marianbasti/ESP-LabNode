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

# Enhanced database setup
def init_db():
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
    conn.commit()
    conn.close()

# API interaction functions
def get_sensor_data(base_url):
    try:
        response = requests.get(f"{base_url}/api/sensor")
        return response.json()
    except:
        return None

def set_relay_state(base_url, state):
    try:
        response = requests.post(f"{base_url}/api/relay", params={"state": state})
        return response.json()
    except:
        return None

def get_timer_config(base_url):
    try:
        response = requests.get(f"{base_url}/api/timer")
        return response.json()
    except:
        return None

def set_timer_config(base_url, config):
    try:
        response = requests.post(f"{base_url}/api/timer", json=config)
        return response.json()
    except:
        return None

def get_hostname(base_url):
    try:
        response = requests.get(f"{base_url}/api/hostname")
        return response.json()
    except:
        return None

def set_hostname(base_url, hostname):
    try:
        response = requests.post(f"{base_url}/api/hostname", json={"hostname": hostname})
        return response.json()
    except:
        return None

# Device management functions
def add_device(name, url):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    try:
        hostname_info = get_hostname(url)
        hostname = hostname_info.get('hostname', 'unknown') if hostname_info else 'unknown'
        c.execute("INSERT INTO devices (name, url, hostname) VALUES (?, ?, ?)",
                 (name, url, hostname))
        conn.commit()
        return True
    except sqlite3.IntegrityError:
        return False
    finally:
        conn.close()

def remove_device(device_id):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    c.execute("DELETE FROM devices WHERE id = ?", (device_id,))
    c.execute("DELETE FROM readings WHERE device_id = ?", (device_id,))
    conn.commit()
    conn.close()

def get_devices():
    conn = sqlite3.connect('sensor_data.db')
    df = pd.read_sql_query("SELECT * FROM devices", conn)
    conn.close()
    return df

# Add new functions for reading frequency management
def update_device_frequency(device_id, frequency):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    c.execute("UPDATE devices SET reading_frequency = ? WHERE id = ?", (frequency, device_id))
    conn.commit()
    conn.close()

# Data storage
def store_reading(device_id, temperature, humidity):
    conn = sqlite3.connect('sensor_data.db')
    c = conn.cursor()
    c.execute("INSERT INTO readings VALUES (?, ?, ?, ?)",
              (datetime.now(), device_id, temperature, humidity))
    conn.commit()
    conn.close()

def get_historical_data(device_id, hours=24):
    conn = sqlite3.connect('sensor_data.db')
    query = f"""
    SELECT timestamp, temperature, humidity 
    FROM readings 
    WHERE device_id = ?
    AND timestamp >= datetime('now', '-{hours} hours')
    """
    df = pd.read_sql_query(query, conn, params=(device_id,), parse_dates=['timestamp'])
    conn.close()
    return df

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
    st.set_page_config(page_title="Temperature Control Dashboard", layout="wide")
    
    if not os.environ.get("GUI_PW"):
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
            st.metric("Temperature", f"{data['temperature']}°C")
            st.metric("Humidity", f"{data['humidity']}%")
            store_reading(selected_device.id, data['temperature'], data['humidity'])
        else:
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

if __name__ == "__main__":
    main()
