import json
import time
import random
import threading
import datetime
import math
import queue
import sys
import paho.mqtt.client as mqtt

# ==========================================
# CONFIGURAÇÕES GLOBAIS DO TES
# ==========================================
BROKER_ADDRESS = "192.168.1.109"      # IP do broker local
BROKER_PORT = 1883                # Porta MQTT
TOPIC = "fabrica/teste/carga"     # Tópico alvo
NUM_CLIENTS = 100                 # Clientes simultâneos
PUBLISH_INTERVAL_SEC = 5       # Intervalo entre envios (em segundos)
QOS_LEVEL = 0                     # QoS 0 (foco em velocidade/latência)
MAX_RUNTIME_SEC = 600            # Tempo máximo de execução em segundos (3600s = 1 hora)

# v2: uso  ->  python gerador_carga_v2.py R10 10   (nome da rodada, nº de clientes)
RODADA = sys.argv[1] if len(sys.argv) > 1 else "teste"
if len(sys.argv) > 2:
    NUM_CLIENTS = int(sys.argv[2])
ARQ_LOG = f"log_dados_enviados_{RODADA}.txt"
ARQ_EVENTOS = f"log_eventos_{RODADA}.txt"
eventos_lock = threading.Lock()


def registra_evento(texto):
    """v2: registra conexões, desconexões e falhas de publicação com horário."""
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    with eventos_lock:
        with open(ARQ_EVENTOS, "a", encoding="utf-8") as f:
            f.write(f"[{ts}] {texto}\n")

# Fila thread-safe para o log, evitando que o IO de disco atrase o MQTT
log_queue = queue.Queue()
tempo_inicio_global = time.time()

def gravador_de_log():
    """
    Thread dedicada exclusivamente a escrever os dados no arquivo TXT.
    Isso impede que a latência do disco afete a precisão do envio MQTT.
    """
    with open(ARQ_LOG, "a", encoding="utf-8") as arquivo_log:
        arquivo_log.write(f"\n--- NOVO TESTE INICIADO EM {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')} ---\n")
        while True:
            mensagem = log_queue.get()
            if mensagem is None:  # Sinal para encerrar a thread
                break
            arquivo_log.write(mensagem + "\n")
            arquivo_log.flush()  # Força a gravação no disco
            log_queue.task_done()

def gerar_dado_realista(client_id, tempo_atual):
    """
    Simula uma assinatura de vibração mecânica realista baseada no tempo.
    Usa ruído gaussiano e harmônicas para imitar máquinas reais.
    """
    t = tempo_atual - tempo_inicio_global
    
    # Define determinísticamente que ~10% das máquinas (ex: finais 7, 8, 9) estão em WARNING
    id_numero = int(client_id.split('_')[-1])
    is_warning = (id_numero % 10) >= 8
    
    if is_warning:
        status = "WARNING"
        base_vib = 4.5  # Nível de vibração RMS alto
        ruido_std = 0.6 # Maior instabilidade
    else:
        status = "NORMAL"
        base_vib = 1.2  # Operação suave
        ruido_std = 0.15
        
    # Adiciona oscilação senoidal para simular variação de carga (período de ~30s)
    variacao_carga = math.sin(2 * math.pi * t / 30.0) * (0.4 if is_warning else 0.1)
    
    # Ruído branco gaussiano (distribuição normal)
    vib_rms = random.gauss(base_vib + variacao_carga, ruido_std)
    
    return round(abs(vib_rms), 4), status

def mqtt_client_task(client_id):
    def on_connect(client, userdata, flags, reason_code, properties):
        registra_evento(f"{client_id} | CONNECT | {reason_code}")
        if reason_code != 0:
            print(f"[{client_id}] Falha na conexão: {reason_code}")

    def on_disconnect(client, userdata, flags, reason_code, properties):
        registra_evento(f"{client_id} | DISCONNECT | {reason_code}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect

    try:
        client.connect(BROKER_ADDRESS, BROKER_PORT, keepalive=60)
        client.loop_start() 
    except Exception as e:
        print(f"[{client_id}] Erro de conexão: {e}")
        return

    # Sincroniza o início para todos os clientes dispararem no mesmo ritmo
    time.sleep(1.0)

    while True:
        tempo_atual = time.time()
        
        # Se o tempo limite foi atingido, a thread morre silenciosamente
        if (tempo_atual - tempo_inicio_global) >= MAX_RUNTIME_SEC:
            break
            
        vib_rms, status = gerar_dado_realista(client_id, tempo_atual)
        
        payload = {
            "client_id": client_id,
            "timestamp_ns": time.time_ns(), # Nanosegundos para cálculo exato de latência
            "vibracao_rms": vib_rms,
            "status_maquina": status
        }
        
        payload_json = json.dumps(payload)
        info = client.publish(TOPIC, payload_json, qos=QOS_LEVEL)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            registra_evento(f"{client_id} | PUBLISH_FALHOU rc={info.rc} | {payload_json}")
        
        # Formata o timestamp de disparo e envia para a fila do gravador TXT
        ts_log = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        log_queue.put(f"[{ts_log}] {client_id} | rc={info.rc} | Payload: {payload_json}")

        # Mantém a precisão do intervalo subtraindo o tempo gasto na execução do loop
        tempo_processamento = time.time() - tempo_atual
        tempo_espera = max(0.0, PUBLISH_INTERVAL_SEC - tempo_processamento)
        time.sleep(tempo_espera)

def main():
    print(f"⚙️ Iniciando Teste de Carga de Alta Precisão MQTT...")
    print(f"📍 Broker: {BROKER_ADDRESS}:{BROKER_PORT} | Rodada: {RODADA} | Clientes: {NUM_CLIENTS}")
    registra_evento(f"INICIO rodada={RODADA} clientes={NUM_CLIENTS} intervalo={PUBLISH_INTERVAL_SEC}s qos={QOS_LEVEL} duracao={MAX_RUNTIME_SEC}s")
    print(f"⏳ Duração programada: {MAX_RUNTIME_SEC} segundos ({MAX_RUNTIME_SEC/60:.1f} minutos)")
    
    # Inicia a thread separada para gravação de log
    thread_log = threading.Thread(target=gravador_de_log, daemon=True)
    thread_log.start()
    
    threads = []
    for i in range(NUM_CLIENTS):
        c_id = f"sensor_virtual_{i+1:03d}"
        t = threading.Thread(target=mqtt_client_task, args=(c_id,), daemon=True)
        threads.append(t)
        t.start()
        time.sleep(0.01) # Ramp-up suave

    print("✅ Sistema em execução! Aguardando tempo limite ou pressione CTRL+C para forçar encerramento.")

    try:
        while True:
            tempo_decorrido = time.time() - tempo_inicio_global
            if tempo_decorrido >= MAX_RUNTIME_SEC:
                print(f"\n⏰ Tempo máximo de {MAX_RUNTIME_SEC}s atingido! Finalizando gravações...")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n🛑 Teste interrompido manualmente. Finalizando gravações...")
        
    # Processo de encerramento seguro (executado tanto no timeout quanto no CTRL+C)
    tempo_total = time.time() - tempo_inicio_global
    
    # Avisa a thread de log para desligar e aguarda terminar a fila
    log_queue.put(None)
    thread_log.join(timeout=3.0)
    
    print(f"⏱️ Tempo total real de operação: {tempo_total:.3f} segundos.")

if __name__ == "__main__":
    main()