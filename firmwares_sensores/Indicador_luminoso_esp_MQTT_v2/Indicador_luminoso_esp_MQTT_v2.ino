#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Configurações de Rede e Broker ---
const char* ssid = "wifi";
const char* password = "senha";
const char* mqtt_server = "192.168.1.108";
const int mqtt_port = 1883;
const char* mqtt_topic = "casa/parametros/sensor1";

// --- Configuração dos Pinos dos LEDs ---
const int LED_VERDE = 4;   // Conforto BOM
const int LED_AMARELO = 18; // Conforto MÉDIO
const int LED_VERMELHO = 21; // Conforto RUIM

WiFiClient espClient;
PubSubClient client(espClient);

// --- Protótipos das Funções ---
void setup_wifi();
void callback(char* topic, byte* payload, unsigned int length);
void reconnect();
void analisarConforto(float temp, float pressao, float umidade);

void setup() {
  Serial.begin(115200);
  
  // Configura pinos dos LEDs como saída
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  
  // Inicializa com todos os LEDs desligados
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARELO, LOW);
  digitalWrite(LED_VERMELHO, LOW);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}

// --- Conexão Wi-Fi ---
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Conectando em ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi conectado!");
  Serial.print("Endereço IP: ");
  Serial.println(WiFi.localIP());
}

// --- Callback: Executado quando chega mensagem no tópico ---
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensagem recebida no tópico: ");
  Serial.println(topic);

  // Converte o payload para string
  String messageBuffer = "";
  for (int i = 0; i < length; i++) {
    messageBuffer += (char)payload[i];
  }
  Serial.print("Payload: ");
  Serial.println(messageBuffer);

  // Aloca o documento JSON (tamanho estimado para o seu JSON)
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, messageBuffer);

  if (error) {
    Serial.print("Falha no parsing do JSON: ");
    Serial.println(error.f_str());
    return;
  }

  // Extrai as variáveis do JSON
  float temperatura = doc["temperatura"];
  float pressao = doc["pressao"];
  float umidade = doc["umidade"];
  long timestamp = doc["timestamp"]; // Se precisar usar o tempo

  // Exibe no monitor serial para validação
  Serial.printf("Dados Extraídos -> Temp: %.2f°C | Pressão: %.2f hPa | Umidade: %.2f%%\n", temperatura, pressao, umidade);

  // Executa o cruzamento de dados para acender o LED correto
  analisarConforto(temperatura, pressao, umidade);
}

// --- Lógica de Cruzamento de Dados (Matriz de Conforto) ---
void analisarConforto(float temp, float pressao, float umidade) {
  int pontosDeDesconforto = 0;

  // 1. Análise da Temperatura (Ideal: 20°C a 25°C)
  if (temp < 18.0 || temp > 28.0) {
    pontosDeDesconforto += 2; // Muito frio ou muito quente
  } else if ((temp >= 18.0 && temp < 20.0) || (temp > 25.0 && temp <= 28.0)) {
    pontosDeDesconforto += 1; // Tolerável
  }

  // 2. Análise da Umidade (Ideal: 40% a 60%)
  if (umidade < 30.0 || umidade > 75.0) {
    pontosDeDesconforto += 2; // Ar muito seco ou muito saturado
  } else if ((umidade >= 30.0 && umidade < 40.0) || (umidade > 60.0 && umidade <= 75.0)) {
    pontosDeDesconforto += 1; // Alerta
  }

  // 3. Análise da Pressão Atmosférica (Média nível do mar: ~1013 hPa)
  // Variações bruscas indicam mudança drástica de tempo (frentes frias/tempestades)
  if (pressao < 1000.0 || pressao > 1025.0) {
    pontosDeDesconforto += 1; 
  }

  // --- Atribuição dos Estados dos LEDs ---
  // Reseta todos
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_AMARELO, LOW);
  digitalWrite(LED_VERMELHO, LOW);

  if (pontosDeDesconforto == 0) {
    // Ambiente perfeito
    digitalWrite(LED_VERDE, HIGH);
    Serial.println("Estado Ambiental: BOM [LED VERDE]");
  } 
  else if (pontosDeDesconforto >= 1 && pontosDeDesconforto <= 2) {
    // Apenas um ou dois parâmetros saíram do ideal levemente
    digitalWrite(LED_AMARELO, HIGH);
    Serial.println("Estado Ambiental: MÉDIO [LED AMARELO]");
  } 
  else {
    // Múltiplos parâmetros ruins ou desvios graves
    digitalWrite(LED_VERMELHO, HIGH);
    Serial.println("Estado Ambiental: RUIM [LED VERMELHO]");
  }
  Serial.println("----------------------------------------------");
}

// --- Reconexão ao Broker MQTT ---
void reconnect() {
  while (!client.connected()) {
    Serial.print("Tentando conexão MQTT...");
    // Cria um ID de cliente único baseado no MAC
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("conectado!");
      // Subscreve no tópico desejado
      client.subscribe(mqtt_topic);
    } else {
      Serial.print("falhou, rc=");
      Serial.print(client.state());
      Serial.println(" Tentando novamente em 5 segundos");
      delay(5000);
    }
  }
}