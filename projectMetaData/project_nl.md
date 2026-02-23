# mqttAerosolSensor

Dit project beschrijft een praktische ESP32 + PMS5003 fijnstofmeter die PM1.0-, PM2.5- en PM10-waarden via MQTT publiceert.

De opzet is bewust eenvoudig gehouden (ESP32 DevKitC + PMS5003 via UART).

Na het aansluiten van de sensor op de juiste 5V-rail werkten de metingen betrouwbaar. Het project verstuurt de data vervolgens naar een MQTT-broker voor integratie met tools zoals Home Assistant of Grafana, zodat trends eenvoudig te visualiseren zijn en automatiseringen kunnen worden getriggerd.

Kortom: dit is een compact makerproject dat eenvoudige hardware combineert met waardevolle, realistische inzichten in de binnenluchtkwaliteit.
