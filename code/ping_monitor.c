/*
 * PROGRAMMA: Ping Monitor Scalabile con Bulk CSV Load
 * 
 * SCOPO PRINCIPALE:
 * Monitorare la latenza di rete verso un grande numero di IP target (fino a 300,000+)
 * utilizzando fping per misurazioni efficienti e caricando i risultati nel database
 * MySQL tramite operazioni bulk CSV invece di singole query.
 * 
 * CARATTERISTICHE CHIAVE:
 * - Gestione efficiente di grandi volumi di IP (fino a 300,000+)
 * - Utilizzo di fping per misurazioni parallele e veloci
 * - Caricamento bulk via CSV invece di singole query INSERT
 * - Persistenza dello stato tra le esecuzioni
 * - Gestione memoria dinamica con ridimensionamento automatico
 * 
 * ARCHITETTURA:
 * 1. Carica IP target da tabella BGP del database
 * 2. Esegue misurazioni ping massive con fping
 * 3. Calcola statistiche (media e deviazione standard)
 * 4. Esporta risultati in file CSV
 * 5. Carica CSV nel database con LOAD DATA INFILE
 * 6. Salva stato per ripartenza rapida
 * 
 * UTILIZZO:
 * ./programma <db_host> <db_user> <db_password> <db_name> <bgp_table>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

// ================================
// COSTANTI DI CONFIGURAZIONE
// ================================

#define INITIAL_MAX_IPS 10000     // Dimensione iniziale array IP
#define PING_SAMPLES 6            // Numero di campioni ping da mantenere per ogni IP
#define CMD_BUFFER_SIZE 1024      // Dimensione buffer per comandi di sistema
#define STATE_FILE "/tmp/ping_monitor_state.dat"  // File per salvare stato tra esecuzioni
#define FPING_TARGETS_FILE "/tmp/fping_targets.txt"  // File temporaneo per lista IP fping
#define CSV_FILE "/tmp/ping_results.csv"  // File CSV per export risultati

// ================================
// STRUTTURE DATI
// ================================

/**
 * Struttura per memorizzare dati ping per un singolo IP target
 * Mantiene uno storico dei campioni e statistiche di aggiornamento
 */
typedef struct {
        char ip[16];                      // Indirizzo IP del target (XXX.XXX.XXX.XXX)
        float ping_times[PING_SAMPLES];   // Array circolare degli ultimi campioni ping
        int current_index;                // Indice corrente nell'array circolare
        int samples_collected;            // Numero totale di campioni validi raccolti
        int needs_first_update;           // Flag: 1 se necessita primo aggiornamento completo
} PingData;

// ================================
// VARIABILI GLOBALI
// ================================

MYSQL *conn;                    // Connessione al database MySQL
PingData *ping_data = NULL;     // Array dinamico di strutture PingData
int ip_count = 0;               // Numero attuale di IP nell'array
int max_ips = 0;                // Capacità massima corrente dell'array

// ================================
// FUNZIONI DI GESTIONE MEMORIA
// ================================

/**
 * Inizializza l'array dinamico per memorizzare i dati ping
 * @param initial_size: Dimensione iniziale dell'array
 * @return: 1 se successo, 0 se errore di allocazione
 */
int init_ping_data(int initial_size) {
        ping_data = malloc(initial_size * sizeof(PingData));
        if (!ping_data) {
                return 0;
        }
        max_ips = initial_size;
        return 1;
}

/**
 * Ridimensiona l'array dinamico per accomodare più IP
 * @param new_size: Nuova dimensione dell'array
 * @return: 1 se successo, 0 se errore di riallocazione
 */
int resize_ping_data(int new_size) {
        PingData *new_data = realloc(ping_data, new_size * sizeof(PingData));
        if (!new_data) {
                return 0;
        }
        ping_data = new_data;
        max_ips = new_size;
        return 1;
}

/**
 * Libera tutta la memoria allocata per i dati ping
 * Resetta anche i contatori
 */
void free_ping_data() {
        if (ping_data) {
                free(ping_data);
                ping_data = NULL;
        }
        ip_count = 0;
        max_ips = 0;
}

/**
 * Salva lo stato corrente del monitoraggio su file
 * Permette di riprendere le misurazioni dopo un riavvio
 */
void save_state() {
        FILE *fp = fopen(STATE_FILE, "wb");
        if (fp) {
                fwrite(&ip_count, sizeof(int), 1, fp);
                for (int i = 0; i < ip_count; i++) {
                        fwrite(&ping_data[i], sizeof(PingData), 1, fp);
                }
                fclose(fp);
        }
}

/**
 * Carica lo stato precedente del monitoraggio da file
 * Ripristina i dati ping e le statistiche dall'ultima esecuzione
 */
void load_state() {
        FILE *fp = fopen(STATE_FILE, "rb");
        if (fp) {
                int saved_count;
                fread(&saved_count, sizeof(int), 1, fp);
                
                // Se necessario, ridimensiona l'array per contenere tutti i dati salvati
                if (saved_count > max_ips) {
                        if (!resize_ping_data(saved_count + 1000)) {
                                printf("Errore: impossibile allocare memoria per stato salvato\n");
                                fclose(fp);
                                return;
                        }
                }
                
                // Legge tutti i dati ping salvati
                for (int i = 0; i < saved_count; i++) {
                        fread(&ping_data[i], sizeof(PingData), 1, fp);
                }
                ip_count = saved_count;
                fclose(fp);
        }
}

// ================================
// FUNZIONI DI GESTIONE DATABASE
// ================================

/**
 * Inizializza la connessione al database MySQL con opzioni ottimizzate
 * @param host: Hostname del server MySQL
 * @param user: Username per l'autenticazione
 * @param password: Password per l'autenticazione
 * @param database: Nome del database da utilizzare
 */
void init_database(const char *host, const char *user, const char *password, const char *database) {
        conn = mysql_init(NULL);
        if (conn == NULL) {
                fprintf(stderr, "Errore nell'inizializzazione del database: %s\n", mysql_error(conn));
                exit(1);
        }

        // Abilita LOCAL INFILE per caricamento CSV (OTTIMIZZAZIONE CHIAVE)
        int enable_local_infile = 1;
        mysql_options(conn, MYSQL_OPT_LOCAL_INFILE, &enable_local_infile);
        
        // Aumenta timeout per gestire grandi dataset
        int connect_timeout = 30;
        int read_timeout = 60;
        int write_timeout = 60;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
        mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);

        if (mysql_real_connect(conn, host, user, password, database, 0, NULL, 0) == NULL) {
                fprintf(stderr, "Errore nella connessione al database: %s\n", mysql_error(conn));
                mysql_close(conn);
                exit(1);
        }
}

/**
 * Crea la tabella per i risultati ping se non esiste
 * Include indici per ottimizzare le query
 * @param table_name: Nome della tabella da creare
 */
void create_ping_table(const char *table_name) {
        char query[512];
        snprintf(query, sizeof(query),
                "CREATE TABLE IF NOT EXISTS %s ("
                "id INT AUTO_INCREMENT PRIMARY KEY, "
                "probe_ip VARCHAR(15) NOT NULL, "           // IP del probe che ha fatto le misurazioni
                "target_ip VARCHAR(15) NOT NULL, "          // IP target misurato
                "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "  // Data/ora misurazione
                "avg_ping DECIMAL(5,2) NOT NULL, "          // Latenza media in millisecondi
                "mdev_ping DECIMAL(5,2) NOT NULL, "         // Deviazione standard della latenza
                "packets_sent INT NOT NULL, "               // Numero di pacchetti inviati
                "UNIQUE KEY unique_measurement (probe_ip, target_ip), "  // Vincolo univoco
                "INDEX idx_timestamp (timestamp), "         // Indice per query temporali
                "INDEX idx_target_ip (target_ip)"           // Indice per ricerca per IP target
                ")", table_name);
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nella creazione della tabella: %s\n", mysql_error(conn));
        }
}

/**
 * Verifica se un IP è già presente nell'array dei target
 * @param ip: Indirizzo IP da cercare
 * @return: 1 se trovato, 0 se non trovato
 */
int ip_exists_in_array(const char *ip) {
        for (int i = 0; i < ip_count; i++) {
                if (strcmp(ping_data[i].ip, ip) == 0) {
                        return 1;
                }
        }
        return 0;
}

/**
 * Carica gli IP target dalla tabella BGP del database
 * Sincronizza la lista locale con il database:
 * - Rimuove IP non più presenti nel database
 * - Aggiunge nuovi IP trovati nel database
 * @param table_name: Nome della tabella BGP da cui caricare gli IP
 */
void load_target_ips(const char *table_name) {
        char query[512];
        snprintf(query, sizeof(query), "SELECT DISTINCT peer_ip FROM %s WHERE peer_ip IS NOT NULL", table_name);
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nel caricamento IP da %s: %s\n", table_name, mysql_error(conn));
                return;
        }
        
        MYSQL_RES *result = mysql_store_result(conn);
        if (result == NULL) {
                fprintf(stderr, "Nessun risultato dalla query su %s\n", table_name);
                return;
        }
        
        int new_ip_count = 0;
        int removed_count = 0;
        MYSQL_ROW row;
        
        printf("Ricerca IP target nella tabella %s...\n", table_name);
        
        // Fase 1: Rimuovi IP che non esistono più nel database
        for (int i = 0; i < ip_count; i++) {
                int still_exists = 0;
                mysql_data_seek(result, 0);  // Riposiziona all'inizio del risultato
                
                while ((row = mysql_fetch_row(result))) {
                        if (row[0] != NULL && strcmp(ping_data[i].ip, row[0]) == 0) {
                                still_exists = 1;
                                break;
                        }
                }
                
                if (!still_exists) {
                        printf("  Rimuovo IP non più presente: %s\n", ping_data[i].ip);
                        // Rimuove l'elemento spostando gli elementi successivi
                        for (int j = i; j < ip_count - 1; j++) {
                                memcpy(&ping_data[j], &ping_data[j + 1], sizeof(PingData));
                        }
                        ip_count--;
                        i--;  // Compensa la rimozione nell'indice del loop
                        removed_count++;
                }
        }
        
        // Fase 2: Aggiungi nuovi IP dal database
        mysql_data_seek(result, 0);
        while ((row = mysql_fetch_row(result))) {
                if (row[0] != NULL && !ip_exists_in_array(row[0])) {
                        // Se l'array è pieno, ridimensionalo
                        if (ip_count >= max_ips) {
                                int new_size = max_ips + 5000;
                                if (!resize_ping_data(new_size)) {
                                        printf("Errore: memoria esaurita, impossibile aggiungere IP %s\n", row[0]);
                                        break;
                                }
                        }
                        
                        // Inizializza nuovo elemento PingData
                        strncpy(ping_data[ip_count].ip, row[0], 15);
                        ping_data[ip_count].ip[15] = '\0';
                        memset(ping_data[ip_count].ping_times, 0, sizeof(ping_data[ip_count].ping_times));
                        ping_data[ip_count].current_index = 0;
                        ping_data[ip_count].samples_collected = 0;
                        ping_data[ip_count].needs_first_update = 1;  // Segnala che serve primo aggiornamento completo
                        
                        new_ip_count++;
                        ip_count++;
                        
                        // Output progressivo (mostra solo primi 10 IP)
                        if (new_ip_count <= 10) {
                                printf("  Aggiunto nuovo IP: %s\n", ping_data[ip_count-1].ip);
                        } else if (new_ip_count == 11) {
                                printf("  ... (altri IP nascosti per brevità)\n");
                        }
                }
        }
        
        mysql_free_result(result);
        
        printf("Caricamento completato: %d nuovi IP aggiunti, %d IP rimossi, totale: %d IP target\n", 
               new_ip_count, removed_count, ip_count);
}

// ================================
// FUNZIONI DI CALCOLO STATISTICHE
// ================================

/**
 * Calcola la media aritmetica di un array di valori
 * @param values: Array di valori float
 * @param count: Numero di elementi nell'array
 * @return: Media dei valori, 0 se l'array è vuoto
 */
float calculate_average(float *values, int count) {
        if (count == 0) return 0;
        float sum = 0;
        for (int i = 0; i < count; i++) {
                sum += values[i];
        }
        return sum / count;
}

/**
 * Calcola la deviazione standard (mean deviation) di un array di valori
 * @param values: Array di valori float
 * @param count: Numero di elementi nell'array
 * @param avg: Media precalcolata dei valori
 * @return: Deviazione standard, 0 se l'array è vuoto
 */
float calculate_mdev(float *values, int count, float avg) {
        if (count == 0) return 0;
        float sum_sq = 0;
        for (int i = 0; i < count; i++) {
                float diff = values[i] - avg;
                sum_sq += diff * diff;
        }
        return sqrt(sum_sq / count);
}

// ================================
// FUNZIONI DI MISURAZIONE PING
// ================================

/**
 * Esegue misurazioni ping massive utilizzando fping
 * Ottimizzato per grandi quantità di IP tramite file di input
 */
void execute_ping_measurements() {
        if (ip_count == 0) {
                printf("Nessun IP target da pingare\n");
                return;
        }
        
        // Crea file temporaneo con lista IP per fping
        FILE *fp = fopen(FPING_TARGETS_FILE, "w");
        if (!fp) {
                perror("Errore creazione file targets");
                return;
        }
        
        printf("Scrivendo %d IP nel file targets per fping...\n", ip_count);
        for (int i = 0; i < ip_count; i++) {
                fprintf(fp, "%s\n", ping_data[i].ip);
        }
        fclose(fp);
        
        // Esegue fping con opzioni ottimizzate per grandi dataset
        char cmd[CMD_BUFFER_SIZE];
        snprintf(cmd, sizeof(cmd), "fping -c 1 -t 400 -i 1 -f %s 2>&1", FPING_TARGETS_FILE);
        // -c 1: Un solo ping per IP
        // -t 400: Timeout di 400ms
        // -i 1: Intervallo di 1ms tra i ping
        // -f: Legge IP da file
        
        printf("Esecuzione fping per %d IP...\n", ip_count);
        FILE *fping_output = popen(cmd, "r");
        if (!fping_output) {
                perror("Errore esecuzione fping");
                remove(FPING_TARGETS_FILE);
                return;
        }
        
        char line[256];
        int processed_count = 0;
        int success_count = 0;
        int fail_count = 0;
        
        // Processa l'output di fping riga per riga
        while (fgets(line, sizeof(line), fping_output)) {
                char ip[16];
                float min, avg, max;
                int xmt, rcv, loss;
                
                // Parsa la riga di output di fping
                if (sscanf(line, "%15s : xmt/rcv/%%loss = %d/%d/%d%%, min/avg/max = %f/%f/%f",
                           ip, &xmt, &rcv, &loss, &min, &avg, &max) == 7) {
                        
                        processed_count++;
                        if (rcv > 0) {
                                success_count++;
                        } else {
                                fail_count++;
                        }
                        
                        // Trova l'IP corrispondente nell'array e aggiorna i dati
                        for (int i = 0; i < ip_count; i++) {
                                if (strcmp(ping_data[i].ip, ip) == 0) {
                                        if (rcv > 0) {
                                                ping_data[i].ping_times[ping_data[i].current_index] = avg;
                                        } else {
                                                ping_data[i].ping_times[ping_data[i].current_index] = -1;  // Segnale di fallimento
                                        }
                                        
                                        // Aggiorna indice array circolare
                                        ping_data[i].current_index = (ping_data[i].current_index + 1) % PING_SAMPLES;
                                        if (ping_data[i].samples_collected < PING_SAMPLES) {
                                                ping_data[i].samples_collected++;
                                        }
                                        break;
                                }
                        }
                }
        }
        
        pclose(fping_output);
        remove(FPING_TARGETS_FILE);  // Pulizia file temporaneo
        
        printf("Risultati fping: %d processati, %d successi, %d falliti\n", 
               processed_count, success_count, fail_count);
}

// ================================
// FUNZIONI DI ESPORTAZIONE CSV E BULK LOAD
// ================================

/**
 * Crea un file CSV con tutti i risultati ping per il caricamento bulk
 * @param probe_ip: Indirizzo IP del probe che ha effettuato le misurazioni
 */
void create_csv_file(const char *probe_ip) {
        printf("Creazione file CSV per %d IP...\n", ip_count);
        
        FILE *csv_fp = fopen(CSV_FILE, "w");
        if (!csv_fp) {
                perror("Errore creazione file CSV");
                return;
        }
        
        int records_written = 0;
        
        // Scrive nel CSV solo gli IP con dati validi e che necessitano aggiornamento
        for (int i = 0; i < ip_count; i++) {
                float valid_samples[PING_SAMPLES];
                int valid_count = 0;
                
                // Filtra solo i campioni ping validi (> 0)
                for (int j = 0; j < ping_data[i].samples_collected; j++) {
                        if (ping_data[i].ping_times[j] > 0) {
                                valid_samples[valid_count++] = ping_data[i].ping_times[j];
                        }
                }
                
                if (valid_count > 0) {
                        float avg = calculate_average(valid_samples, valid_count);
                        float mdev = calculate_mdev(valid_samples, valid_count, avg);
                        
                        int should_update = 0;
                        
                        // Logica di aggiornamento: primo aggiornamento completo o aggiornamenti successivi
                        if (ping_data[i].needs_first_update) {
                                if (ping_data[i].samples_collected >= PING_SAMPLES) {
                                        should_update = 1;
                                        ping_data[i].needs_first_update = 0;  // Reset flag primo aggiornamento
                                }
                        } else {
                                should_update = 1;
                        }
                        
                        if (should_update) {
                                // Scrivi riga CSV nel formato: probe_ip,target_ip,avg_ping,mdev_ping,packets_sent
                                fprintf(csv_fp, "%s,%s,%.2f,%.2f,%d\n", 
                                        probe_ip, ping_data[i].ip, avg, mdev, valid_count);
                                records_written++;
                        }
                }
        }
        
        fclose(csv_fp);
        printf("Scritti %d record nel file CSV\n", records_written);
}

/**
 * Carica il file CSV nel database usando operazioni bulk ottimizzate
 * Utilizza tabella temporanea e LOAD DATA INFILE per massime performance
 * @param table_name: Nome della tabella destinazione
 */
void bulk_load_to_database(const char *table_name) {
        printf("Caricamento bulk dei dati nel database...\n");
        
        // Crea tabella temporanea per staging dei dati
        char temp_table_query[512];
        snprintf(temp_table_query, sizeof(temp_table_query),
                 "CREATE TEMPORARY TABLE temp_ping_data ("
                 "probe_ip VARCHAR(15) NOT NULL, "
                 "target_ip VARCHAR(15) NOT NULL, "
                 "avg_ping DECIMAL(5,2) NOT NULL, "
                 "mdev_ping DECIMAL(5,2) NOT NULL, "
                 "packets_sent INT NOT NULL, "
                 "PRIMARY KEY (probe_ip, target_ip))");  // Chiave primaria per UPSERT
        
        if (mysql_query(conn, temp_table_query)) {
                fprintf(stderr, "Errore creazione tabella temporanea: %s\n", mysql_error(conn));
                return;
        }
        
        // CARICAMENTO BULK: Carica il CSV nella tabella temporanea
        // Questa è l'ottimizzazione principale che sostituisce 300,000 query individuali
        char load_query[1024];
        snprintf(load_query, sizeof(load_query),
                 "LOAD DATA LOCAL INFILE '%s' "
                 "INTO TABLE temp_ping_data "
                 "FIELDS TERMINATED BY ',' "
                 "LINES TERMINATED BY '\\n' "
                 "(probe_ip, target_ip, avg_ping, mdev_ping, packets_sent)", 
                 CSV_FILE);
        
        if (mysql_query(conn, load_query)) {
                fprintf(stderr, "Errore caricamento CSV: %s\n", mysql_error(conn));
                // Pulizia tabella temporanea anche in caso di errore
                mysql_query(conn, "DROP TEMPORARY TABLE IF EXISTS temp_ping_data");
                return;
        }
        
        // UPSERT: Trasferisce dati dalla temporanea alla tabella principale
        // Se esiste già un record con stesso (probe_ip, target_ip), lo aggiorna
        char upsert_query[1024];
        snprintf(upsert_query, sizeof(upsert_query),
                 "INSERT INTO %s (probe_ip, target_ip, avg_ping, mdev_ping, packets_sent) "
                 "SELECT probe_ip, target_ip, avg_ping, mdev_ping, packets_sent "
                 "FROM temp_ping_data "
                 "ON DUPLICATE KEY UPDATE "
                 "avg_ping = VALUES(avg_ping), "
                 "mdev_ping = VALUES(mdev_ping), "
                 "packets_sent = VALUES(packets_sent), "
                 "timestamp = CURRENT_TIMESTAMP", table_name);
        
        if (mysql_query(conn, upsert_query)) {
                fprintf(stderr, "Errore upsert dati: %s\n", mysql_error(conn));
        } else {
                // Mostra statistiche dell'operazione bulk
                my_ulonglong affected_rows = mysql_affected_rows(conn);
                printf("Bulk load completato: %llu record aggiornati/inseriti\n", affected_rows);
        }
        
        // Pulizia: elimina tabella temporanea
        mysql_query(conn, "DROP TEMPORARY TABLE IF EXISTS temp_ping_data");
}

/**
 * Funzione principale per aggiornare il database con i risultati ping
 * Coordina la creazione CSV e il caricamento bulk
 * @param probe_ip: IP del probe per identificare la sorgente delle misurazioni
 * @param table_name: Tabella destinazione per i risultati
 */
void update_database_results(const char *probe_ip, const char *table_name) {
        printf("Aggiornamento database con %d IP...\n", ip_count);
        
        // Crea file CSV
        create_csv_file(probe_ip);
        
        // Carica in bulk nel database
        bulk_load_to_database(table_name);
        
        // Rimuovi file CSV temporaneo
        remove(CSV_FILE);
}

/**
 * Mostra l'utilizzo di memoria corrente del programma
 * Utile per monitorare le risorse con grandi dataset
 */
void print_memory_usage() {
        printf("Utilizzo memoria: %.2f MB per %d IP\n", 
               (ip_count * sizeof(PingData)) / (1024.0 * 1024.0), ip_count);
}

// ================================
// FUNZIONE MAIN
// ================================

/**
 * Funzione principale del programma
 * Coordina tutte le fasi del monitoraggio ping
 */
int main(int argc, char *argv[]) {
        printf("=== PING MONITOR SCALABILE (Bulk CSV Load) ===\n");
        
        // Verifica parametri di input
        if (argc != 6) {
                fprintf(stderr, "Uso: %s <db_host> <db_user> <db_password> <db_name> <bgp_table>\n", argv[0]);
                return 1;
        }
        
        // Inizializza memoria dinamica per i dati ping
        if (!init_ping_data(INITIAL_MAX_IPS)) {
                fprintf(stderr, "Errore: impossibile allocare memoria iniziale\n");
                return 1;
        }
        
        // Parsa parametri da command line
        const char *db_host = argv[1];
        const char *db_user = argv[2];
        const char *db_password = argv[3];
        const char *db_name = argv[4];
        const char *bgp_table = argv[5];
        
        // Determina automaticamente l'IP del probe (macchina corrente)
        char probe_ip[16] = "unknown";
        FILE *ip_cmd = popen("ip route get 8.8.8.8 | grep -oP 'src \\K[^ ]+'", "r");
        if (ip_cmd) {
                if (fgets(probe_ip, sizeof(probe_ip), ip_cmd)) {
                        probe_ip[strcspn(probe_ip, "\n")] = 0;  // Rimuovi newline
                }
                pclose(ip_cmd);
        }
        
        // Stampa configurazione
        printf("Parametri:\n");
        printf("  DB_HOST: %s\n", db_host);
        printf("  DB_USER: %s\n", db_user);
        printf("  DB_NAME: %s\n", db_name);
        printf("  BGP_TABLE: %s\n", bgp_table);
        printf("  PROBE_IP: %s\n", probe_ip);
        
        // Carica stato precedente (se esiste)
        load_state();
        printf("Stato caricato: %d IP in memoria\n", ip_count);
        
        // Inizializza database e strutture
        init_database(db_host, db_user, db_password, db_name);
        create_ping_table("ping_results");
        load_target_ips(bgp_table);
        
        print_memory_usage();
        
        // Esegue misurazioni solo se ci sono IP target
        if (ip_count > 0) {
                execute_ping_measurements();
                update_database_results(probe_ip, "ping_results");
                save_state();
                printf("Stato salvato per prossima esecuzione\n");
        } else {
                printf("Nessun IP target trovato nella tabella %s\n", bgp_table);
        }
        
        // Pulizia finale
        mysql_close(conn);
        free_ping_data();
        printf("=== COMPLETATO ===\n");
        return 0;
}
