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
DB_TABLE="${DB_TABLE:-bgp_connections}"
RS_PROGRAM="${RS_PROGRAM:-get_connections}"

echo "=== CONFIGURAZIONE ==="
echo "Database: $DB_NAME"
echo "Utente: $DB_USER"
echo "Programma C: $RS_PROGRAM"
echo "======================"

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
        
        for cmd in gcc mysql; do
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
                return 1
        fi
        return 0
}

# Compilazione programma C
compile_c_program() {
        echo "Compilazione programma C..."
        
        if ! check_dependencies; then
                echo "Impossibile compilare a causa di dipendenze mancanti"
                exit 1
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
        
        if gcc $mysql_cflags -o "$RS_PROGRAM" get_ip.c $mysql_libs; then
                echo "Programma C compilato con successo e salvato come '$RS_PROGRAM'"
                return 0
        else
                echo "Errore nella compilazione del programma C"
                return 1
        fi
}

# Creazione database e tabelle (se non esistenti)
setup_database() {
        echo "Verifica esistenza database $DB_NAME..."
        CREATE_DB_QUERY="CREATE DATABASE IF NOT EXISTS $DB_NAME;"
        if execute_query "$CREATE_DB_QUERY"; then
                echo "Database verificato/creato con successo"
        else
                echo "Errore nella creazione del database"
                return 1
        fi

        CREATE_TABLE_QUERY="
        USE $DB_NAME;
        CREATE TABLE IF NOT EXISTS $DB_TABLE (
                id INT AUTO_INCREMENT PRIMARY KEY,
                protocol_name VARCHAR(100) NOT NULL,
                peer_ip VARCHAR(15) NOT NULL,
                last_seen DATETIME NOT NULL,
                UNIQUE KEY unique_connection (protocol_name, peer_ip),
                INDEX idx_protocol_name (protocol_name),
                INDEX idx_peer_ip (peer_ip),
                INDEX idx_last_seen (last_seen)
        );"
        
        if execute_query "$CREATE_TABLE_QUERY"; then
                echo "Tabella verificata/creata con successo!"
                return 0
        else
                echo "Errore nella creazione della tabella"
                return 1
        fi
}


# Esecuzione singola monitoraggio
run_single_monitoring() {
        echo "Esecuzione monitoraggio BGP..."
	
	# Controllo errori vari	
        if [[ ! -f "./$RS_PROGRAM" ]]; then
                echo "Programma C non trovato. Compilazione automatica..."
                if ! compile_c_program; then
                        echo "Impossibile compilare il programma C"
                        return 1
                fi
        fi
        
        if [[ ! -f "./$RS_PROGRAM" ]]; then
                echo "Errore: Programma C './$RS_PROGRAM' non trovato dopo la compilazione"
                return 1
        fi
        
        local password_param="${DB_PASSWORD:-}"
        
        # Comando per ottenere l'output da parsare nel programma C
        sudo birdc show protocols all | ./"$RS_PROGRAM" "$DB_HOST" "$DB_USER" "$password_param" "$DB_NAME"
        
	# Stampa risultati
        local result=$?
        if [[ $result -eq 0 ]]; then
                echo "Monitoraggio completato con successo"
                return 0
        else
                echo "Errore durante il monitoraggio (codice: $result)"
                return 1
        fi
}

# Funzione per attendere fino al prossimo intervallo di 10 secondi
wait_for_next_interval() {
        local current_second=$(date +%S)
        
        # Rimuovi eventuali zeri iniziali per evitare interpretazione ottale
        current_second=$((10#$current_second))
        
        local target_second
        
        # Calcola il prossimo target sincronizzandosi con i secondi (05, 15, 25, 35, 45, 55)
        if [[ $current_second -lt 5 ]]; then
                target_second=5
        elif [[ $current_second -lt 15 ]]; then
                target_second=15
        elif [[ $current_second -lt 25 ]]; then
                target_second=25
        elif [[ $current_second -lt 35 ]]; then
                target_second=35
        elif [[ $current_second -lt 45 ]]; then
                target_second=45
        elif [[ $current_second -lt 55 ]]; then
                target_second=55
        else
	# Se siamo a 55 o oltre, il prossimo target � 5 del minuto successivo
                target_second=5
        fi
        
        # Calcola i secondi da attendere
        local seconds_to_wait=$((target_second - current_second))
        if [[ $seconds_to_wait -lt 0 ]]; then
                seconds_to_wait=$((60 - current_second + target_second))
        fi
        
        echo $seconds_to_wait
}

# Esecuzione monitoraggio periodico sincronizzato
run_synchronized_monitoring() {
        echo "=== AVVIO MONITORAGGIO PERIODICO SINCRONIZZATO ==="
        echo "Esecuzioni ai secondi: 05, 15, 25, 35, 45, 55"
        echo "Orario sistema: $(date '+%H:%M:%S')"
        echo "Premi Ctrl+C per interrompere"
        echo "==================================================="
        
        # Loop periodico infinito
        while true; do
                local wait_time=$(wait_for_next_interval)
                local next_time=$(date -d "+${wait_time} seconds" '+%H:%M:%S')
                
                echo "[$(date '+%H:%M:%S')] Prossima esecuzione alle ${next_time} (tra ${wait_time} secondi)..."
                sleep $wait_time
                
                run_single_monitoring
                echo "-------------------------------------"
        done
}

# Menu principale
main() {
        echo "=== MONITORAGGIO PERIODICO SINCRONIZZATO ===" 
        check_dependencies && compile_c_program && setup_database && run_synchronized_monitoring
}

main "$@"
