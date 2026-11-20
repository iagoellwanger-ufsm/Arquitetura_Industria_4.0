import time
import json
import requests
import xml.etree.ElementTree as ET
import paho.mqtt.client as mqtt  # requer paho-mqtt 2.x

# --- Configurações ---
AGENT_URL = "http://localhost:5000"
INTERVALO_S = 15

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPIC = "tcc/bancada/vibracao"
SENSOR_NAME = "acelerometro_bancada_01"

# dataItemId de cada eixo no Devices.xml -> nome do eixo no payload
EIXOS = {"acc_x": "x", "acc_y": "y", "acc_z": "z"}
TIPOS_ACELERACAO = ("Acceleration", "AccelerationTimeSeries")

# connect_async + loop_start: conecta em segundo plano e reconecta sozinho,
# mesmo que o broker ainda não esteja no ar quando o script inicia.
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="Python_Edge_Node")
mqtt_client.connect_async(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()


def nome_tag(elemento):
    """Nome da tag sem o namespace (funciona com Agent 1.x e 2.x)."""
    return elemento.tag.split("}")[-1]


def consultar(caminho, params=None):
    """Faz o GET no Agent e devolve o Header e a raiz do XML. Levanta erro se vier MTConnectError."""
    resposta = requests.get(f"{AGENT_URL}/{caminho}", params=params, timeout=5)
    raiz = ET.fromstring(resposta.content)
    for el in raiz.iter():
        if nome_tag(el) == "Error":
            raise RuntimeError(f"Agent: {el.attrib.get('errorCode')} {(el.text or '').strip()}")
        if nome_tag(el) == "Header":
            return el, raiz
    raise RuntimeError("Resposta do Agent sem Header")


def ler_janela(seq):
    """Filtra tudo que chegou no Agent desde 'seq': fica só com a aceleração dos 3 eixos.

    Pagina até alcançar o fim do buffer, então a janela cobre todos os dados
    desde a última leitura, sem depender de quantas observações chegaram.
    """
    valores = {data_id: [] for data_id in EIXOS}
    ultimo_ts = ""
    while True:
        header, raiz = consultar("sample", {"from": seq, "count": 1000})
        seq = int(header.attrib["nextSequence"])
        for obs in raiz.iter():
            data_id = obs.attrib.get("dataItemId")
            texto = (obs.text or "").strip()
            if data_id in valores and nome_tag(obs) in TIPOS_ACELERACAO and texto and texto != "UNAVAILABLE":
                valores[data_id].extend(float(v) for v in texto.split())
                ultimo_ts = max(ultimo_ts, obs.attrib.get("timestamp", ""))
        if seq > int(header.attrib["lastSequence"]):
            return valores, ultimo_ts, seq


def calcular_rms(valores):
    """Remove o offset DC e calcula o RMS da vibração."""
    media = sum(valores) / len(valores)
    return (sum((v - media) ** 2 for v in valores) / len(valores)) ** 0.5


def main():
    print(f"Edge Node iniciado. Enviando RMS a cada {INTERVALO_S}s para {MQTT_TOPIC}.")
    seq = None  # None = precisa sincronizar com o Agent

    while True:
        inicio = time.monotonic()
        try:
            if seq is None:
                header, _ = consultar("current")
                seq = int(header.attrib["nextSequence"])
                print(f"[{time.strftime('%H:%M:%S')}] Sincronizado com o Agent na sequência {seq}.")
            else:
                valores, timestamp, seq = ler_janela(seq)

                if all(valores.values()):
                    rms = {EIXOS[data_id]: calcular_rms(v) for data_id, v in valores.items()}
                    rms_global = (rms["x"] ** 2 + rms["y"] ** 2 + rms["z"] ** 2) ** 0.5
                    payload = json.dumps({
                        "sensor": SENSOR_NAME,
                        "timestamp": timestamp,  # horário da última medição, vindo do Agent
                        "aceleracao_rms_x": round(rms["x"], 4),
                        "aceleracao_rms_y": round(rms["y"], 4),
                        "aceleracao_rms_z": round(rms["z"], 4),
                        "aceleracao_rms_global": round(rms_global, 4),
                    })
                    info = mqtt_client.publish(MQTT_TOPIC, payload, qos=0)
                    status = "MQTT TX" if info.rc == mqtt.MQTT_ERR_SUCCESS else "MQTT NÃO ENVIADO (broker desconectado)"
                    print(f"[{time.strftime('%H:%M:%S')}] {status} -> {payload}")
                else:
                    contagem = {EIXOS[k]: len(v) for k, v in valores.items()}
                    print(f"[{time.strftime('%H:%M:%S')}] Faltam dados de algum eixo nesta janela. Amostras: {contagem}")

        except Exception as e:
            # Agent fora do ar, rede, ou sequência fora do buffer (OUT_OF_RANGE): ressincroniza
            print(f"[{time.strftime('%H:%M:%S')}] {type(e).__name__}: {e}. Ressincronizando...")
            seq = None

        # Mantém o ritmo de 15s descontando o tempo gasto na leitura
        time.sleep(max(0, INTERVALO_S - (time.monotonic() - inicio)))


if __name__ == "__main__":
    main()