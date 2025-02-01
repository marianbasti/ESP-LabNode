
# Setup
### ESP32
Primero cambiamos la configuración de red en `sdkconfig`
```
CONFIG_WIFI_SSID="your_ssid_here"
CONFIG_WIFI_PASSWORD="your_password_here"
```
y luego compilamos y flasheamos main.c al ESP32.
### Web UI
Setear contraseña en variable de entorno:
```
GUI_PW=password1234
```
Instalamos las dependencias de python:
```
pip install -r requirements.txt
```
Luego levantamos el server
```
streamlit run dashboard.py
```
---
| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- |
