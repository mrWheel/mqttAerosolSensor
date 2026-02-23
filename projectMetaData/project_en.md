# mqttAerosolSensor

This project describes a practical ESP32 + PMS5003 fine-dust monitor that publishes PM1.0, PM2.5, and PM10 values via MQTT.

The setup is intentionally simple (ESP32 DevKitC + PMS5003 over UART).

After wiring the sensor to the proper 5V rail, measurements worked reliably. The project then sends data to an MQTT broker for integration with tools like Home Assistant or Grafana, making it easy to visualize trends and trigger automations.

In short, this is a compact maker project that combines straightforward hardware with meaningful real-world insights about indoor air quality.
