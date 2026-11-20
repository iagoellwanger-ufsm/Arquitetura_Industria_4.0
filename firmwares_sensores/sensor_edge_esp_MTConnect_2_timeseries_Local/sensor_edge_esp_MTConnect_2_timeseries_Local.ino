/*
 * Adapter MTConnect - Assinatura de vibração (TIME_SERIES) com MPU-6050
 *
 * A cada INTERVALO_JANELAS_MS, captura N_AMOSTRAS de aceleração (X, Y, Z)
 * a TAXA_HZ usando o FIFO interno do sensor (amostragem precisa, sem jitter
 * do Wi-Fi) e envia cada eixo como uma linha SHDR TIME_SERIES:
 *
 *   |acc_x|256|1000|v1 v2 v3 ... v256
 *          ^    ^
 *          |    taxa de amostragem (Hz)
 *          quantidade de amostras
 *
 * Valores em mm/s^2 (unidade MTConnect para ACCELERATION).
 */

#include <WiFi.h>
#include <Wire.h>

const char* ssid = "Rede_Local"; // COLOQUE SEU WI-FI AQUI
const char* password = "Senha123"; // COLOQUE SUA SENHA AQUI

// ================== IP FIXO ==================
IPAddress local_IP(192, 168, 137, 100);
IPAddress gateway (192, 168, 137, 1);loc
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns1    (8, 8, 8, 8);
IPAddress dns2    (8, 8, 4, 4);

WiFiServer server(7878);

// ================== SENSOR / AQUISIÇÃO ==================
const int MPU_ADDR = 0x68;

const int  N_AMOSTRAS = 256;     // amostras por janela (potência de 2 facilita FFT depois)
const int  TAXA_HZ    = 1000;    // taxa de amostragem: 1000, 500, 250, 200, 125, 100...
const unsigned long INTERVALO_JANELAS_MS = 1000;  // tempo entre o início de cada janela

// Fundo de escala do acelerômetro: 0 = ±2g, 1 = ±4g, 2 = ±8g, 3 = ±16g
// ±2g satura fácil em máquina vibrando (a gravidade já consome 1g no eixo vertical)
const uint8_t ESCALA = 1;
const float LSB_POR_G[] = {16384.0, 8192.0, 4096.0, 2048.0};
const float G_MM_S2 = 9806.65;

// Registradores do MPU-6050
const uint8_t REG_SMPLRT_DIV   = 0x19;
const uint8_t REG_CONFIG       = 0x1A;
const uint8_t REG_ACCEL_CONFIG = 0x1C;
const uint8_t REG_FIFO_EN      = 0x23;
const uint8_t REG_INT_STATUS   = 0x3A;
const uint8_t REG_USER_CTRL    = 0x6A;
const uint8_t REG_PWR_MGMT_1   = 0x6B;
const uint8_t REG_FIFO_COUNT_H = 0x72;
const uint8_t REG_FIFO_R_W     = 0x74;

// Buffers da janela (valores brutos)
int16_t ax[N_AMOSTRAS], ay[N_AMOSTRAS], az[N_AMOSTRAS];

// Buffer para montar a linha SHDR (até ~10 caracteres por valor)
char linha[N_AMOSTRAS * 10 + 64];

// ================== LED DE STATUS ==================
const int LED_PIN = 2;

enum Status {
  ST_ERRO_SENSOR,       // pisca muito rápido (50 ms)
  ST_CONECTANDO_WIFI,   // pisca rápido (200 ms)
  ST_AGUARDANDO_AGENT,  // pisca lento (1 s)
  ST_AGENT_CONECTADO    // aceso fixo
};
Status status = ST_CONECTANDO_WIFI;

void atualizaLed() {
  static unsigned long ultimo = 0;
  static bool estado = false;
  unsigned long intervalo = 1000;

  switch (status) {
    case ST_AGENT_CONECTADO:
      digitalWrite(LED_PIN, HIGH);
      return;
    case ST_ERRO_SENSOR:      intervalo = 50;   break;
    case ST_CONECTANDO_WIFI:  intervalo = 200;  break;
    case ST_AGUARDANDO_AGENT: intervalo = 1000; break;
  }

  if (millis() - ultimo >= intervalo) {
    ultimo = millis();
    estado = !estado;
    digitalWrite(LED_PIN, estado);
  }
}

void esperaComLed(unsigned long ms) {
  unsigned long inicio = millis();
  while (millis() - inicio < ms) {
    atualizaLed();
    delay(5);
  }
}

// ================== I2C AUXILIAR ==================
byte escreveReg(uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(valor);
  return Wire.endTransmission();
}

uint8_t leReg(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 1, true);
  return Wire.available() ? Wire.read() : 0;
}

uint16_t leFifoCount() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_FIFO_COUNT_H);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 2, true);
  if (Wire.available() < 2) return 0;
  uint8_t h = Wire.read();
  uint8_t l = Wire.read();
  return ((uint16_t)h << 8) | l;
}

void resetaFifo() {
  escreveReg(REG_USER_CTRL, 0x04);  // FIFO_RESET
  escreveReg(REG_USER_CTRL, 0x40);  // FIFO_EN
  leReg(REG_INT_STATUS);            // limpa flag de overflow
}

// ================== CONFIGURAÇÃO DO SENSOR ==================
bool configuraSensor() {
  // Acorda o chip usando o PLL do giroscópio como clock (mais estável que o interno)
  if (escreveReg(REG_PWR_MGMT_1, 0x01) != 0) return false;
  delay(100);

  // Filtro passa-baixa digital: DLPF_CFG = 1 -> acelerômetro ~184 Hz, base de 1 kHz
  // (corta frequências acima da metade da taxa e reduz aliasing)
  escreveReg(REG_CONFIG, 0x01);

  // Taxa de amostragem = 1 kHz / (1 + SMPLRT_DIV)
  escreveReg(REG_SMPLRT_DIV, (1000 / TAXA_HZ) - 1);

  // Fundo de escala
  escreveReg(REG_ACCEL_CONFIG, ESCALA << 3);

  // FIFO recebe só o acelerômetro (6 bytes por amostra)
  escreveReg(REG_FIFO_EN, 0x08);
  resetaFifo();

  return true;
}

// ================== AQUISIÇÃO DE UMA JANELA ==================
// Retorna true se capturou N_AMOSTRAS completas sem perder dados
bool adquireJanela() {
  resetaFifo();  // começa a janela com o FIFO vazio

  int n = 0;
  unsigned long inicio = millis();
  unsigned long timeout = (1000UL * N_AMOSTRAS / TAXA_HZ) + 500;

  while (n < N_AMOSTRAS) {
    if (millis() - inicio > timeout) {
      Serial.println("Timeout na aquisicao");
      return false;
    }
    if (leReg(REG_INT_STATUS) & 0x10) {   // FIFO_OFLOW_INT
      Serial.println("Overflow no FIFO");
      return false;
    }

    int disponiveis = leFifoCount() / 6;
    if (disponiveis == 0) {
      delayMicroseconds(500);
      continue;
    }

    // Lê em lotes de até 20 amostras (120 bytes, cabe no buffer do Wire)
    int lote = disponiveis;
    if (lote > 20) lote = 20;
    if (lote > N_AMOSTRAS - n) lote = N_AMOSTRAS - n;

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(REG_FIFO_R_W);
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, lote * 6, true);
    if (Wire.available() < lote * 6) return false;

    for (int i = 0; i < lote; i++) {
      uint8_t b[6];
      for (int k = 0; k < 6; k++) b[k] = Wire.read();
      ax[n] = (int16_t)((b[0] << 8) | b[1]);
      ay[n] = (int16_t)((b[2] << 8) | b[3]);
      az[n] = (int16_t)((b[4] << 8) | b[5]);
      n++;
    }
    atualizaLed();
  }
  return true;
}

// ================== ENVIO SHDR ==================
// Formato TIME_SERIES: |nome|quantidade|taxa|v1 v2 v3 ...
void enviaSerie(WiFiClient& client, const char* nome, const int16_t* dados) {
  const float fator = G_MM_S2 / LSB_POR_G[ESCALA];
  int pos = snprintf(linha, sizeof(linha), "|%s|%d|%d|", nome, N_AMOSTRAS, TAXA_HZ);

  for (int i = 0; i < N_AMOSTRAS && pos < (int)sizeof(linha) - 16; i++) {
    pos += snprintf(linha + pos, sizeof(linha) - pos,
                    i ? " %.1f" : "%.1f", dados[i] * fator);
  }
  linha[pos++] = '\n';
  client.write((const uint8_t*)linha, pos);
}

// ================== COMANDOS DO AGENT (heartbeat) ==================
// O Agent envia "* PING"; respondemos "* PONG <ms>" para ele detectar quedas rápido
void trataComandos(WiFiClient& client) {
  static char cmd[64];
  static int len = 0;

  while (client.available()) {
    char c = client.read();
    if (c == '\n' || c == '\r') {
      cmd[len] = '\0';
      if (strncmp(cmd, "* PING", 6) == 0) {
        client.print("* PONG 10000\n");
      }
      len = 0;
    } else if (len < (int)sizeof(cmd) - 1) {
      cmd[len++] = c;
    }
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(1000);
  Serial.println("\nIniciando Adapter de vibracao (TIME_SERIES)...");

  Wire.begin(21, 22);
  Wire.setClock(400000);  // I2C rápido: necessário para ler 1 kHz com folga

  if (configuraSensor()) {
    Serial.println("Sensor MPU-6050 configurado!");
  } else {
    Serial.println("Falha fisica no cabeamento do MPU-6050.");
    status = ST_ERRO_SENSOR;
    while (1) { esperaComLed(1000); }
  }

  Serial.println("Ligando antena Wi-Fi...");
  status = ST_CONECTANDO_WIFI;
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet, dns1, dns2)) {
    Serial.println("Falha ao configurar IP fixo!");
  }
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    esperaComLed(1000);
    Serial.println("Conectando ao WiFi...");
  }

  server.begin();
  status = ST_AGUARDANDO_AGENT;
  Serial.println("\n--- TUDO PRONTO ---");
  Serial.print("Adapter MTConnect aguardando Agent no IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Janela: %d amostras a %d Hz (%.0f ms), a cada %lu ms\n",
                N_AMOSTRAS, TAXA_HZ, 1000.0 * N_AMOSTRAS / TAXA_HZ, INTERVALO_JANELAS_MS);
}

// ================== LOOP ==================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    status = ST_CONECTANDO_WIFI;
    Serial.println("WiFi caiu, reconectando...");
    WiFi.reconnect();
    while (WiFi.status() != WL_CONNECTED) {
      esperaComLed(500);
    }
    Serial.print("WiFi reconectado. IP: ");
    Serial.println(WiFi.localIP());
  }

  status = ST_AGUARDANDO_AGENT;
  atualizaLed();

  WiFiClient client = server.available();

  if (client) {
    Serial.println("MTConnect Agent conectado!");
    status = ST_AGENT_CONECTADO;
    atualizaLed();
    client.setNoDelay(true);
    client.print("|avail|AVAILABLE\n");

    unsigned long ultimaJanela = millis() - INTERVALO_JANELAS_MS;  // primeira janela imediata

    while (client.connected()) {
      trataComandos(client);

      if (millis() - ultimaJanela >= INTERVALO_JANELAS_MS) {
        ultimaJanela = millis();

        if (adquireJanela()) {
          enviaSerie(client, "acc_x", ax);
          enviaSerie(client, "acc_y", ay);
          enviaSerie(client, "acc_z", az);
        } else {
          Serial.println("Janela descartada.");
        }
      }
      delay(1);
    }
    client.stop();
    Serial.println("Agent desconectado.");
  }
}
