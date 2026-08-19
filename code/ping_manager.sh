#!/bin/bash

# File di configurazione
CONFIG_FILE="latenza_config.conf"

# Verifica che il file di configurazione esista
if [[ ! -f "$CONFIG_FILE" ]]; then
        echo "Errore: File di configurazione '$CONFIG_FILE' non trovato!"
        exit 1
fi

# Caricamento parametri dal file di configurazione
source "$CONFIG_FILE"

# Imposta valori di default
DB_NAME="${DB_NAME:-latenza_router}"
DB_USER="${DB_USER:-bgp_monitor}"
DB_PASSWORD="${DB_PASSWORD:-}"
DB_HOST="${DB_HOST:-localhost}"
BGPCONN_TABLE="${BGPCONN_TABLE:-bgp_connections}"
PING_TABLE="${PING_TABLE:-ping_results}"
PING_PROGRAM="${PING_PROGRAM:-ping_monitor}"
PING_INTERVAL=10  # Fisso a 10 secondi
PING_COUNT="${PING_COUNT:-6}"  # 6 campioni per la media

echo "=== CONFIGURAZIONE PING MANAGER ==="
echo "Database: $DB_NAME"
echo "Host: $DB_HOST"
echo "Utente: $DB_USER"
echo "Intervallo ping: $PING_INTERVAL secondi"
echo "Campioni per media: $PING_COUNT"
echo "Programma C: $PING_PROGRAM"
echo "==================================="

# Funzione per eseguire query sul database
execute_query() {
        local query="$1"
        if [[ -z "$DB_PASSWORD" ]]; then
                mysql -h "$DB_HOST" -u "$DB_USER" -e "$query" 2>/dev/null
        else
                mysql -h "$DB_HOST" -u "$DB_USER" -p"$DB_PASSWORD" -e "$query" 2>/dev/null
        fi
}

# Verifica dipendenze
check_dependencies() {
        local missing_deps=()
        
        for cmd in gcc mysql fping; do
                if ! command -v "$cmd" >/dev/null 2>&1; then
                        missing_deps+=("$cmd")
                fi
        done
        
        if [[ ! -f "/usr/include/mysql/mysql.h" ]] && \
           [[ ! -f "/usr/local/include/mysql/mysql.h" ]] && \
           [[ ! -f "/usr/include/mariadb/mysql.h" ]]; then
                missing_deps+=("libmysqlclient-dev")
        fi
        
        if [[ ${#missing_deps[@]} -gt 0 ]]; then
                echo "Dipendenze mancanti: ${missing_deps[*]}"
                echo "Installa con: sudo apt-get install ${missing_deps[*]}"
                return 1
        fi
        return 0
}

# Compilazione programma C
compile_ping_program() {
        echo "Compilazione programma ping..."
        
        if ! check_dependencies; then
                echo "Impossibile compilare a causa di dipendenze mancanti"
                return 1
        fi
        
        local mysql_cflags
        local mysql_libs
        
        if command -v mysql_config >/dev/null 2>&1; then
                mysql_cflags=$(mysql_config --cflags)
                mysql_libs=$(mysql_config --libs)
                echo "Usando mysql_config per le flags di compilazione"
        else
                mysql_cflags="-I/usr/include/mysql"
                mysql_libs="-lmysqlclient"
                echo "Usando flags di compilazione predefinite"
        fi
        
        if gcc $mysql_cflags -o "$PING_PROGRAM" ping_monitor.c $mysql_libs -lm; then
                echo "Programma ping compilato con successo"
                return 0
        else
                echo "Errore nella compilazione del programma ping"
                return 1
        fi
}

# Calcola quando eseguire il prossimo ping (sincronizzato con secondi 00, 10, 20, 30, 40, 50)
get_next_ping_time() {
        local current_seconds=$(date +%S)
        
        # Rimuovi eventuali zeri iniziali per evitare interpretazione ottale
        current_seconds=$((10#$current_seconds))
        
        local seconds_to_wait=$(( (10 - (current_seconds % 10)) % 10 ))
        
        echo $seconds_to_wait
}

# Esegue un ciclo di ping
run_ping_cycle() {
        local cycle_count=$1
        local start_time=$(date +%s)
        
        echo "[$(date '+%H:%M:%S')] Ciclo ping $cycle_count - Esecuzione misurazioni..."
        
        local password_param=""
        if [[ -n "$DB_PASSWORD" ]]; then
                password_param="$DB_PASSWORD"
        fi
        
        ./"$PING_PROGRAM" "$DB_HOST" "$DB_USER" "$password_param" "$DB_NAME" "$BGPCONN_TABLE"
        
        local result=$?
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        
        if [[ $result -eq 0 ]]; then
                echo "[$(date '+%H:%M:%S')] Ciclo ping $cycle_count completato (durata: ${duration}s)"
        else
                echo "[$(date '+%H:%M:%S')] Errore nel ciclo ping $cycle_count (code: $result, durata: ${duration}s)"
        fi
        
        return $result
}

# Pulisce lo stato alla chiusura
cleanup() {
        echo ""
        echo "[$(date '+%H:%M:%S')] Ricevuto segnale di interruzione..."
        echo "Pulizia stato..."
        rm -f /tmp/ping_monitor_state.dat
        echo "Ping Manager terminato."
        exit 0
}

# Gestione principale del ping manager
main_ping_manager() {
        echo "=== AVVIO PING MANAGER ==="
        echo "Ping ogni: $PING_INTERVAL secondi (sincronizzato con 00, 10, 20, 30, 40, 50)"
        echo "Campioni per media: $PING_COUNT"
        echo "Primo aggiornamento dopo: $((PING_INTERVAL * PING_COUNT)) secondi"
        echo "Premi Ctrl+C per interrompere"
        echo "==========================="
        
        # Compila il programma se necessario
        if [[ ! -f "./$PING_PROGRAM" ]]; then
                echo "Compilazione programma ping..."
                if ! compile_ping_program; then
                        echo "Impossibile compilare il programa ping"
                        return 1
                fi
        fi
        
        # Verifica che il programma C esista
        if [[ ! -f "./$PING_PROGRAM" ]]; then
                echo "Errore: Programma $PING_PROGRAM non trovato"
                return 1
        fi
        
        local cycle=0
        
        # Registra cleanup per Ctrl+C
        trap cleanup INT TERM
        
        # Allineamento iniziale al prossimo multiplo di 10 secondi
        local initial_wait=$(get_next_ping_time)
        echo "[$(date '+%H:%M:%S')] Allineamento iniziale: attesa di $initial_wait secondi"
        sleep $initial_wait
        
        # Loop principale
        while true; do
                local cycle_start=$(date +%s)
                
                run_ping_cycle $((cycle + 1))
                ((cycle++))
                
                # Calcola tempo di attesa per il prossimo ciclo (mantenendo sincronizzazione)
                local cycle_end=$(date +%s)
                local cycle_duration=$((cycle_end - cycle_start))
                local wait_time=$((PING_INTERVAL - cycle_duration))
                
                if [[ $wait_time -gt 0 ]]; then
                        local next_time=$(date -d "+${wait_time} seconds" '+%H:%M:%S')
                        echo "[$(date '+%H:%M:%S')] Prossimo ping alle $next_time (tra $wait_time secondi)"
                        sleep $wait_time
                else
                        echo "[$(date '+%H:%M:%S')] ATTENZIONE: Ciclo durato troppo ($cycle_duration secondi), continuando immediatamente"
                fi
                
                echo "----------------------------------------"
        done
}

# Avvio programma
main() {
        if ! check_dependencies; then
                exit 1
        fi
        
        # Verifica connessione al database
        echo "Verifica connessione al database..."
        if execute_query "USE $DB_NAME; SELECT 1;" > /dev/null 2>&1; then
                echo "Connessione al database OK"
        else
                echo "ERRORE: Impossibile connettersi al database $DB_NAME"
                exit 1
        fi
        
        # Verifica che la tabella bgp_connections esista e abbia dati
        echo "Verifica tabella $BGPCONN_TABLE..."
        local record_count=$(execute_query "USE $DB_NAME; SELECT COUNT(*) FROM $BGPCONN_TABLE;" 2>/dev/null | tail -1)
        
        if [[ "$record_count" =~ ^[0-9]+$ ]] && [[ $record_count -gt 0 ]]; then
                echo "Tabella $BGPCONN_TABLE OK - $record_count record trovati"
        else
                echo "ERRORE: Tabella $BGPCONN_TABLE vuota o inesistente"
                exit 1
        fi
        
        main_ping_manager
}

main "$@"
