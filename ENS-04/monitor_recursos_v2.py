"""
Monitor de recursos v2 (ENS-04).
Diferenças em relação ao monitor_recursos_F5.py:
  - lista no início TODOS os processos encontrados para cada alvo (nome e PID),
    e avisa quando um alvo não foi encontrado;
  - inclui influxdb3.exe, Grafana e o agente MTConnect entre os alvos;
  - reavalia a lista de processos a cada amostra (pega processos reiniciados);
  - grava, a cada amostra, os 5 processos que mais consumiram CPU na estação
    (arquivo top5_<rodada>.csv), para identificar o consumo fora dos alvos.

Uso (PowerShell, na estação de serviços, de preferência como Administrador):
    pip install psutil
    python monitor_recursos_v2.py R100
    ... rode o teste de carga ...
    Ctrl+C para parar -> gera recursos_R100.csv e top5_R100.csv

Todos os percentuais de CPU estão na mesma base: 0-100 % da capacidade total
da estação (tempo de CPU do processo dividido pelo número de núcleos lógicos).
"""
import csv
import sys
import time
from datetime import datetime

import psutil

# Confira os nomes no Gerenciador de Tarefas (aba Detalhes) e ajuste se preciso.
ALVOS = {
    "mosquitto": ["mosquitto.exe"],
    "nodered": ["node.exe"],
    "influxdb": ["influxdb3.exe", "influxd.exe"],
    "sqlserver": ["sqlservr.exe"],
    "grafana": ["grafana.exe", "grafana-server.exe"],
    "mtconnect": ["agent.exe"],
}
INTERVALO_S = 1.0
N_CPUS = psutil.cpu_count()
rodada = sys.argv[1] if len(sys.argv) > 1 else "teste"

exe2alvo = {exe: alvo for alvo, exes in ALVOS.items() for exe in exes}
vistos = {}  # pid -> Process (mantém o contador de CPU entre amostras)


def amostra():
    """Devolve lista de (nome_exe, pid, cpu_pct_normalizado, mem_mb)."""
    saida = []
    vivos = set()
    for p in psutil.process_iter(["name"]):
        vivos.add(p.pid)
        try:
            if p.pid not in vistos:
                vistos[p.pid] = p
                p.cpu_percent(None)  # primeira leitura só inicia o contador
                continue
            q = vistos[p.pid]
            cpu = q.cpu_percent(None) / N_CPUS
            mem = q.memory_info().rss / 1024 / 1024
            saida.append(((p.info["name"] or "?").lower(), p.pid, cpu, mem))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    for pid in list(vistos):
        if pid not in vivos:
            del vistos[pid]
    return saida


print(f"Nucleos logicos: {N_CPUS}")
amostra()
psutil.cpu_percent(None)
time.sleep(INTERVALO_S)
inicial = amostra()
for alvo, exes in ALVOS.items():
    achados = [(n, pid) for n, pid, _, _ in inicial if n in exes]
    if achados:
        print(f"{alvo:10s}: " + ", ".join(f"{n} (PID {pid})" for n, pid in achados))
    else:
        print(f"{alvo:10s}: *** NENHUM PROCESSO ENCONTRADO *** ({', '.join(exes)})")

arq_rec = f"recursos_{rodada}.csv"
arq_top = f"top5_{rodada}.csv"
with open(arq_rec, "w", newline="", encoding="utf-8") as fr, \
        open(arq_top, "w", newline="", encoding="utf-8") as ft:
    wr, wt = csv.writer(fr), csv.writer(ft)
    cab = ["timestamp", "n_cpus", "cpu_total_pct"]
    for alvo in ALVOS:
        cab += [f"{alvo}_cpu_pct", f"{alvo}_mem_mb", f"{alvo}_n_proc"]
    cab += ["outros_cpu_pct"]
    wr.writerow(cab)
    wt.writerow(["timestamp", "posicao", "processo", "pid", "cpu_pct", "mem_mb"])
    print(f"Gravando em {arq_rec} e {arq_top}. Ctrl+C para parar.")
    try:
        while True:
            time.sleep(INTERVALO_S)
            agora = datetime.now().isoformat(timespec="milliseconds")
            total = psutil.cpu_percent(None)
            dados = amostra()
            linha = [agora, N_CPUS, total]
            soma_alvos = 0.0
            for alvo in ALVOS:
                sel = [d for d in dados if exe2alvo.get(d[0]) == alvo]
                cpu = sum(d[2] for d in sel)
                soma_alvos += cpu
                linha += [round(cpu, 2), round(sum(d[3] for d in sel), 1), len(sel)]
            linha.append(round(max(total - soma_alvos, 0.0), 2))
            wr.writerow(linha)
            fr.flush()
            # "system idle process" (PID 0) não é consumo real
            reais = [d for d in dados if d[1] != 0]
            for i, d in enumerate(sorted(reais, key=lambda x: -x[2])[:5], 1):
                wt.writerow([agora, i, d[0], d[1], round(d[2], 2), round(d[3], 1)])
            ft.flush()
    except KeyboardInterrupt:
        print(f"\nFim. Arquivos salvos: {arq_rec} e {arq_top}")
