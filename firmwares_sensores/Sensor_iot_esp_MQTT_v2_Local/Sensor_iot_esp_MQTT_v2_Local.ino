#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>   // --- Biblioteca nativa para manipulação de tempo ---
#include <stdarg.h> // --- Para a função de log com formatação (printf) ---

// --- Configurações do Wi-Fi ---
const char* WIFI_SSID = "Rede_Local";  // COLOQUE SEU WI-FI AQUI     
const char* WIFI_PASSWORD = "Senha123"; // COLOQUE SUA SENHA AQUI

// --- Configurações do MQTT ---
const char* MQTT_BROKER = "192.168.137.1"; 
const int MQTT_PORT = 1883;
const char* TOPICO_DADOS = "casa/parametros/sensor1"; 

// --- Configurações do Servidor NTP ---
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "a.st1.ntp.br";    // Servidor NTP brasileiro (NIC.br)
const char* ntpServer3 = "time.google.com";
const long  gmtOffset_sec = -10800; // Ajuste para o seu fuso horário (Ex: -3 horas = -3 * 3600 = -10800)
const int   daylightOffset_sec = 0;  // Ajuste de horário de verão (0 se não houver)

// --- Pinos ---
#define I2C_SDA 21
#define I2C_SCL 22
#define LED_STATUS 2  // LED nativo do ESP32 (GPIO 2)

Adafruit_BME280 bme;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long ultimoEnvio = 0;
const long intervalo = 5000; 

// --- Variáveis de controle do LED sem travamento ---
unsigned long tempoUltimoPisca = 0;
int estagioPisca = 0;

// --- Controle do NTP ---
bool ntpSincronizado = false;
unsigned long ultimaConfigNTP = 0;
const unsigned long INTERVALO_RETENTATIVA_NTP = 30000; // Reenvia a config do NTP a cada 30 s se não sincronizar

// =====================================================================
//  TEMPO E LOG
// =====================================================================

// --- Retorna o timestamp Epoch atual (segundos) ou 0 se o NTP ainda não sincronizou ---
// Usa time() direto: não bloqueia (getLocalTime() espera até 5 s quando não há hora válida).
// Depois da primeira sincronização, o relógio interno do ESP32 continua contando
// mesmo com o Wi-Fi fora do ar.
unsigned long getTimestamp() {
  time_t agora = time(nullptr);
  return (agora > 1600000000) ? (unsigned long)agora : 0;
}

// --- Converte o timestamp Epoch em data/hora legível (horário local) ---
String formatarDataHora(unsigned long epoch) {
  if (epoch == 0) {
    return "NTP nao sincronizado";
  }
  time_t t = (time_t)epoch;
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);  // Aplica o fuso configurado em configTime()

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

// --- Carimbo de tempo para o log ---
// Com NTP sincronizado: data/hora real. Antes disso: tempo desde que a placa ligou.
String carimboTempo() {
  unsigned long epoch = getTimestamp();
  if (epoch != 0) {
    return formatarDataHora(epoch);
  }
  unsigned long s = millis() / 1000;
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "uptime %02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  return String(buffer);
}

// --- Log no Serial com carimbo de tempo em TODA linha (uso igual ao printf) ---
void logSerial(const char* formato, ...) {
  char mensagem[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(mensagem, sizeof(mensagem), formato, args);
  va_end(args);
  Serial.printf("[%s] %s\n", carimboTempo().c_str(), mensagem);
}

// =====================================================================
//  CONEXÕES
// =====================================================================

// --- Configura o NTP e espera até 10 s pela primeira sincronização ---
void configurarNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
  ultimaConfigNTP = millis();

  if (ntpSincronizado) return; // Relógio já ajustado antes: não precisa esperar

  logSerial("NTP configurado, aguardando sincronizacao...");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    ntpSincronizado = true;
    logSerial("NTP sincronizado. Relogio ajustado.");
  } else {
    logSerial("NTP ainda nao respondeu. Nova tentativa em 30 s.");
  }
}

void conectarWiFi() {
  logSerial("Conectando em: %s", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(250);
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    logSerial("Wi-Fi conectado! IP: %s", WiFi.localIP().toString().c_str());
    
    configurarNTP();
  } else {
    logSerial("Falha ao conectar no Wi-Fi. Nova tentativa em 10 s.");
  }
}

void conectarMQTT() {
  if (!mqttClient.connected()) {
    logSerial("Tentando conexão MQTT...");
    String clientId = "ESP32Client-" + String(random(0, 0xffff), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      logSerial("Conectado ao Broker MQTT!");
    } else {
      logSerial("Falha no MQTT, erro=%d. Nova tentativa em 10 s.", mqttClient.state());
    }
  }
}

// --- Detecta e registra no Serial o momento exato de quedas e retornos ---
void monitorarConexoes() {
  static bool wifiAnterior = false;
  static bool mqttAnterior = false;
  static unsigned long inicioQuedaWifi = 0;
  static unsigned long inicioQuedaMqtt = 0;

  // Wi-Fi
  bool wifiAtual = (WiFi.status() == WL_CONNECTED);
  if (wifiAtual != wifiAnterior) {
    if (!wifiAtual) {
      inicioQuedaWifi = millis();
      logSerial("ALERTA: Wi-Fi caiu!");
    } else if (inicioQuedaWifi != 0) {
      logSerial("Wi-Fi restabelecido apos %lu s fora do ar.", (millis() - inicioQuedaWifi) / 1000);
    }
    wifiAnterior = wifiAtual;
  }

  // MQTT
  bool mqttAtual = mqttClient.connected();
  if (mqttAtual != mqttAnterior) {
    if (!mqttAtual) {
      inicioQuedaMqtt = millis();
      logSerial("ALERTA: conexao MQTT caiu! (estado=%d)", mqttClient.state());
    } else if (inicioQuedaMqtt != 0) {
      logSerial("MQTT restabelecido apos %lu s fora do ar.", (millis() - inicioQuedaMqtt) / 1000);
    }
    mqttAnterior = mqttAtual;
  }

  // NTP (avisa uma única vez quando o relógio é ajustado)
  if (!ntpSincronizado && getTimestamp() != 0) {
    ntpSincronizado = true;
    logSerial("NTP sincronizado. Relogio ajustado.");
  }
}

// =====================================================================
//  LED DE STATUS
// =====================================================================

void gerenciarLedStatus() {
  unsigned long tempoAtual = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    if (tempoAtual - tempoUltimoPisca >= (estagioPisca == 4 ? 1200 : 1500)) { 
      if (tempoAtual - tempoUltimoPisca >= 150) { 
        tempoUltimoPisca = tempoAtual;
        estagioPisca++;
        if (estagioPisca > 4) estagioPisca = 0;
        
        if (estagioPisca == 0 || estagioPisca == 2) digitalWrite(LED_STATUS, HIGH);
        else digitalWrite(LED_STATUS, LOW);
      }
    }
  } 
  else if (!mqttClient.connected()) {
    if (tempoAtual - tempoUltimoPisca >= 150) {
      tempoUltimoPisca = tempoAtual;
      estagioPisca++;
      if (estagioPisca > 6) estagioPisca = 0;
      
      if (estagioPisca == 0 || estagioPisca == 2 || estagioPisca == 4) digitalWrite(LED_STATUS, HIGH);
      else digitalWrite(LED_STATUS, LOW);
    }
  } 
  else {
    if (tempoAtual - tempoUltimoPisca >= 1000) {
      tempoUltimoPisca = tempoAtual;
      digitalWrite(LED_STATUS, !digitalRead(LED_STATUS)); 
    }
  }
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);

  logSerial("Iniciando estacao BME280...");

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x76)) {
    logSerial("ERRO: sensor BME280 nao encontrado!");
    while (1) {
      digitalWrite(LED_STATUS, HIGH); 
    }
  }
  logSerial("Sensor BME280 OK.");

  conectarWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
}

void loop() {
  gerenciarLedStatus();
  monitorarConexoes();

  static unsigned long ultimaChecagemConexao = 0;
  if (millis() - ultimaChecagemConexao >= 10000) { 
    ultimaChecagemConexao = millis();
    if (WiFi.status() != WL_CONNECTED) {
      conectarWiFi();
    } else {
      // Wi-Fi ok mas relógio ainda sem hora: reenvia a configuração do NTP
      if (!ntpSincronizado && millis() - ultimaConfigNTP >= INTERVALO_RETENTATIVA_NTP) {
        logSerial("Reenviando configuracao do NTP...");
        configurarNTP();
      }
      if (!mqttClient.connected()) {
        conectarMQTT();
      }
    }
  }
  
  mqttClient.loop();

  unsigned long agora = millis();
  if (agora - ultimoEnvio >= intervalo) {
    ultimoEnvio = agora;

    if (WiFi.status() == WL_CONNECTED && mqttClient.connected() && getTimestamp() == 0) {
      logSerial("Leitura nao enviada: aguardando sincronizacao do NTP.");
    }
    else if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
      float temp = bme.readTemperature();
      float pres = bme.readPressure() / 100.0F;
      float umid = bme.readHumidity();
      unsigned long epochTime = getTimestamp(); // Obtém o timestamp atual (segundos)

      StaticJsonDocument<200> doc; 
      doc["temperatura"] = serialized(String(temp, 2)); 
      doc["pressao"]     = serialized(String(pres, 2));
      doc["umidade"]     = serialized(String(umid, 2));
      doc["timestamp"]   = epochTime; // Timestamp Unix em segundos

      String jsonBuffer;
      serializeJson(doc, jsonBuffer);

      if (mqttClient.publish(TOPICO_DADOS, jsonBuffer.c_str())) {
        logSerial("Dados enviados via MQTT | Epoch: %lu | Payload: %s", epochTime, jsonBuffer.c_str());
      } else {
        logSerial("ERRO: falha ao publicar no MQTT.");
      }
    }
  }
}
