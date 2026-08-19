/* 
 * PROGRAMMA: Monitoraggio Sessioni BGP e Generazione Filtri Basati su Latenza
 * 
 * SCOPO PRINCIPALE:
 * 1. Leggere l'output di "birdc show protocols all" per individuare sessioni BGP stabilite
 * 2. Salvare le sessioni attive in un database MySQL
 * 3. Rimuovere dal database le sessioni non più attive
 * 4. Calcolare le latenze tra i router utilizzando dati ping
 * 5. Generare filtri BIRD che impostano le preferenze BGP in base alla latenza
 * 
 * FUNZIONALITÀ CHIAVE:
 * - Monitoraggio continuo dello stato delle sessioni BGP
 * - Pulizia automatica delle sessioni obsolete
 * - Ottimizzazione del routing basata sulle prestazioni di rete
 * - Generazione automatica della configurazione BIRD
 * 
 * UTILIZZO TIPICO:
 * Il programma viene eseguito periodicamente (es. ogni 10 minuti) per mantenere
 * aggiornato il database e rigenerare i filtri di ottimizzazione.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql/mysql.h>
#include <unistd.h>
#include <ctype.h>

// ================================
// COSTANTI DI CONFIGURAZIONE
// ================================

// Dimensione buffer per leggere righe dall'output di birdc
#define MAX_LINE_LENGTH 1024

// Lunghezza massima per il nome di un protocollo BGP
#define MAX_PROTOCOL_NAME 50

// Lunghezza massima per un indirizzo IP (supporta IPv6)
#define MAX_IP_LENGTH 46  // 45 caratteri + null terminator 

// Numero massimo di sessioni BGP che possono essere gestite contemporaneamente
#define MAX_PROTOCOLS 800

// Lunghezza per timestamp nel formato database (YYYY-MM-DD HH:MM:SS)
#define MAX_TIMESTAMP_LENGTH 20

// Numero massimo di rotte che possono essere esportate da un protocollo BGP
#define MAX_ROUTES 400000

// Numero massimo di router unici a cui vengono esportate rotte
#define MAX_EXPORTED_ROUTERS 1000

// Numero massimo di probe (sorgenti ping) che possono essere utilizzati
#define MAX_PROBES 100

// Numero massimo di regole di filtro che possono essere generate
#define MAX_FILTER_RULES 80000

// Comando BIRD da utilizzare (cerca nel PATH di sistema)
#define BIRD_COMMAND "birdc" 

// ================================
// STRUTTURE DATI
// ================================

/**
 * Struttura per memorizzare informazioni su un protocollo BGP
 * Contiene tutti i dati necessari per identificare e monitorare una sessione BGP
 */
typedef struct {
        char name[MAX_PROTOCOL_NAME];      // Nome del protocollo (es: "PEER_AS1234")
        char proto_type[20];               // Tipo di protocollo (solo "BGP" ci interessa)
        char state[20];                    // Stato del protocollo (UP/DOWN)
        char peer_ip[MAX_IP_LENGTH];       // Indirizzo IP del peer BGP remoto
        int is_bgp;                        // Flag: 1 se è un protocollo BGP, 0 altrimenti
        int is_up;                         // Flag: 1 se il protocollo è UP, 0 se DOWN
        int bgp_established;               // Flag: 1 se la sessione BGP è in stato Established
} ProtocolInfo;

/**
 * Struttura per una regola di filtro BIRD
 * Definisce come le rotte da un certo router dovrebbero essere preferite
 */
typedef struct {
        char source_ip[MAX_IP_LENGTH];     // IP del router che annuncia le rotte
        char target_ip[MAX_IP_LENGTH];     // IP del router di destinazione
        double total_latency;              // Latenza totale calcolata (in millisecondi)
        int preference;                    // Preferenza BGP calcolata (1-255)
} FilterRule;

/**
 * Struttura per memorizzare i risultati dei ping
 * Utilizzata per cache delle misurazioni di latenza
 */
typedef struct {
        char probe_ip[MAX_IP_LENGTH];      // IP del probe che ha effettuato il ping
        char target_ip[MAX_IP_LENGTH];     // IP del target pingato
        double avg_ping;                   // Latenza media misurata (ms)
} PingResult;

// ================================
// VARIABILI GLOBALI
// ================================

MYSQL *conn;  // Connessione al database MySQL

// Cache per i risultati ping - ottimizza l'accesso ai dati di latenza
PingResult *ping_cache = NULL;
int ping_cache_size = 0;      // Numero attuale di elementi in cache
int ping_cache_capacity = 0;  // Capacità massima della cache

// ================================
// FUNZIONI DI GESTIONE DATABASE
// ================================

/**
 * Inizializza la connessione al database MySQL
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

        // Imposta opzioni per performance - riconnessione automatica
        mysql_options(conn, MYSQL_OPT_RECONNECT, &(int){1});
        
        if (mysql_real_connect(conn, host, user, password, database, 0, NULL, 0) == NULL) {
                fprintf(stderr, "Errore nella connessione al database: %s\n", mysql_error(conn));
                mysql_close(conn);
                exit(1);
        }
        
        printf("DEBUG: Connessione al database stabilita con successo\n");
}

/**
 * Inserisce o aggiorna un protocollo BGP nel database
 * Utilizza INSERT ... ON DUPLICATE KEY UPDATE per gestire aggiornamenti
 * @param protocol_name: Nome univoco del protocollo BGP
 * @param peer_ip: Indirizzo IP del peer BGP remoto
 */
void insert_protocol_data(const char *protocol_name, const char *peer_ip) {
        char query[512];
        char timestamp[MAX_TIMESTAMP_LENGTH];
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        
        // Formatta il timestamp corrente
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
        
        // Query che inserisce nuovo record o aggiorna timestamp se esiste già
        snprintf(query, sizeof(query),
                 "INSERT INTO bgp_connections (protocol_name, peer_ip, last_seen) "
                 "VALUES ('%s', '%s', '%s') "
                 "ON DUPLICATE KEY UPDATE protocol_name = VALUES(protocol_name), last_seen = VALUES(last_seen)",
                 protocol_name, peer_ip, timestamp);
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nell'inserimento dei dati: %s\n", mysql_error(conn));
        }
}

/**
 * Rimuove dal database i protocolli che non sono più attivi
 * Mantiene solo i protocolli presenti nella lista degli attuali
 * @param active_protocols: Array dei protocolli attualmente attivi
 * @param active_count: Numero di protocolli attivi
 */
void cleanup_old_protocols(ProtocolInfo *active_protocols, int active_count) {
        char query[4096]; // Buffer grande per lista protocolli
        char protocol_list[3072] = ""; // Buffer per lista nomi protocolli
        size_t current_len = 0;
        
        // Se non ci sono protocolli attivi, cancella tutto
        if (active_count == 0) {
                snprintf(query, sizeof(query), "DELETE FROM bgp_connections");
        } else {
                // Costruisce lista di protocolli attivi per la query SQL
                for (int i = 0; i < active_count; i++) {
                        if (active_protocols[i].is_up && active_protocols[i].bgp_established) {
                                size_t needed = strlen(active_protocols[i].name) + 5; // nome + "', '"
                                
                                // Controllo sicurezza per evitare overflow buffer
                                if (current_len + needed >= sizeof(protocol_list) - 1) {
                                        fprintf(stderr, "AVVISO: Buffer protocol_list quasi pieno, tronco la lista\n");
                                        break;
                                }
                                
                                // Aggiungi separatore se non è il primo elemento
                                if (current_len > 0) {
                                        strcat(protocol_list, "', '");
                                        current_len += 4;
                                }
                                strcat(protocol_list, active_protocols[i].name);
                                current_len += strlen(active_protocols[i].name);
                        }
                }
                
                // Costruisce query finale
                if (strlen(protocol_list) == 0) {
                        snprintf(query, sizeof(query), "DELETE FROM bgp_connections");
                } else {
                        snprintf(query, sizeof(query),
                                 "DELETE FROM bgp_connections WHERE protocol_name NOT IN ('%s')",
                                 protocol_list);
                }
        }
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nella pulizia dei vecchi protocolli: %s\n", mysql_error(conn));
        } else {
                printf("DEBUG: Pulizia completata. Protocolli non attivi rimossi.\n");
        }
}

// ================================
// FUNZIONI DI GESTIONE LATENZA
// ================================

/**
 * Carica tutti i risultati ping dal database in una cache in memoria
 * Ottimizza le successive ricerche di latenza evitando query ripetute
 * @return: Numero di record caricati in cache, 0 in caso di errore
 */
int load_ping_cache() {
        printf("DEBUG: Caricamento cache risultati ping...\n");
        
        const char *query = "SELECT probe_ip, target_ip, avg_ping FROM ping_results";
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nel caricamento ping cache: %s\n", mysql_error(conn));
                return 0;
        }
        
        MYSQL_RES *result = mysql_store_result(conn);
        if (result == NULL) {
                fprintf(stderr, "Errore nel store result ping cache: %s\n", mysql_error(conn));
                return 0;
        }
        
        int num_rows = mysql_num_rows(result);
        printf("DEBUG: Trovati %d record ping nel database\n", num_rows);
        
        if (num_rows == 0) {
                printf("DEBUG: Nessun dato ping trovato nel database. I filtri di latenza verranno generati al prossimo ciclo.\n");
                mysql_free_result(result);
                return 0;
        }
        
        // Alloca memoria per la cache con margine di crescita
        ping_cache_capacity = num_rows + 100;
        ping_cache = malloc(ping_cache_capacity * sizeof(PingResult));
        if (ping_cache == NULL) {
                fprintf(stderr, "Errore nell'allocazione della ping cache\n");
                mysql_free_result(result);
                return 0;
        }
        
        // Copia tutti i risultati nella cache
        MYSQL_ROW row;
        ping_cache_size = 0;
        while ((row = mysql_fetch_row(result))) {
                if (ping_cache_size >= ping_cache_capacity) break;
                
                strncpy(ping_cache[ping_cache_size].probe_ip, row[0] ? row[0] : "", MAX_IP_LENGTH - 1);
                strncpy(ping_cache[ping_cache_size].target_ip, row[1] ? row[1] : "", MAX_IP_LENGTH - 1);
                ping_cache[ping_cache_size].avg_ping = row[2] ? atof(row[2]) : 0.0;
                
                ping_cache_size++;
        }
        
        mysql_free_result(result);
        printf("DEBUG: Cache ping caricata con %d elementi\n", ping_cache_size);
        return ping_cache_size;
}

/**
 * Cerca un valore di ping nella cache
 * Ricerca lineare ottimizzata con early exit
 * @param probe_ip: IP del probe che ha effettuato la misurazione
 * @param target_ip: IP del target misurato
 * @return: Valore di latenza in ms, -1.0 se non trovato
 */
double find_ping_value(const char *probe_ip, const char *target_ip) {
        // Ricerca lineare ma con early exit quando possibile
        for (int i = 0; i < ping_cache_size; i++) {
                if (strcmp(ping_cache[i].probe_ip, probe_ip) == 0 && 
                    strcmp(ping_cache[i].target_ip, target_ip) == 0) {
                        return ping_cache[i].avg_ping;
                }
        }
        return -1.0; // Non trovato
}

/**
 * Calcola la latenza minima tra due router considerando tutti i probe disponibili
 * Utilizza la formula: latenza_min = min_over_probes(lat(probe->router1) + lat(probe->router2))
 * @param router1_ip: IP del primo router
 * @param router2_ip: IP del secondo router
 * @return: Latenza minima calcolata in ms, 2.0 come valore di default se nessun dato disponibile
 */
double calculate_min_latency(const char *router1_ip, const char *router2_ip) {
        double min_total_latency = -1.0;
        
        // Ottieni tutti i probe IP unici dalla cache
        char unique_probes[MAX_PROBES][MAX_IP_LENGTH];
        int probe_count = 0;
        
        for (int i = 0; i < ping_cache_size && probe_count < MAX_PROBES; i++) {
                int found = 0;
                for (int j = 0; j < probe_count; j++) {
                        if (strcmp(unique_probes[j], ping_cache[i].probe_ip) == 0) {
                                found = 1;
                                break;
                        }
                }
                if (!found) {
                        strncpy(unique_probes[probe_count], ping_cache[i].probe_ip, MAX_IP_LENGTH - 1);
                        unique_probes[probe_count][MAX_IP_LENGTH - 1] = '\0';
                        probe_count++;
                }
        }
        
        if (probe_count == 0) {
                return 2.0; // Valore di default
        }
        
        // Per ogni probe, calcola la latenza totale router1->probe + probe->router2
        for (int i = 0; i < probe_count; i++) {
                const char *probe_ip = unique_probes[i];
                
                double latency1 = find_ping_value(probe_ip, router1_ip);
                double latency2 = find_ping_value(probe_ip, router2_ip);
                
                if (latency1 >= 0 && latency2 >= 0) {
                        double total_latency = latency1 + latency2;
                        
                        // Aggiorna il minimo trovato
                        if (min_total_latency < 0 || total_latency < min_total_latency) {
                                min_total_latency = total_latency;
                        }
                }
        }
        
        if (min_total_latency >= 0) {
                return min_total_latency;
        } else {
                return 2.0; // Valore di default
        }
}

/**
 * Calcola la preferenza BGP basata sulla latenza
 * Formula: preference = 100 - 50 * ((latency - 1.0) / 49.0) per latenze tra 1-50ms
 * @param latency: Latenza in millisecondi
 * @return: Valore di preferenza BGP (1-255)
 */
int calculate_preference(double latency) {
        int preference;
        
        if (latency <= 1.0) {
                preference = 100;  // Latenza ottimale, massima preferenza
        } else if (latency > 50.0) {
                preference = 50;   // Latenza elevata, preferenza minima
        } else {
                // Scala lineare tra 1ms e 50ms
                preference = (int)(100 - 50 * ((latency - 1.0) / 49.0));
        }
        
        // Assicurati che la preference sia nel range valido (1-255 per BGP local_pref)
        if (preference < 1) preference = 1;
        if (preference > 255) preference = 255;
        
        return preference;
}

// ================================
// FUNZIONI DI INTERAZIONE CON BIRD
// ================================

/**
 * Verifica se il comando birdc è disponibile nel sistema
 * @return: 1 se disponibile, 0 altrimenti
 */
int check_birdc_available() {
        int result = system("which birdc > /dev/null 2>&1");
        return (result == 0);
}

/**
 * Esegue un comando birdc per ottenere le rotte esportate da un protocollo
 * @param protocol_name: Nome del protocollo BGP da interrogare
 * @param route_count: Puntatore per restituire il numero di rotte trovate
 * @return: Array di stringhe con le rotte, NULL in caso di errore
 */
char** execute_birdc_command(const char *protocol_name, int *route_count) {
        // Verifica preliminare disponibilità comando
        if (!check_birdc_available()) {
                fprintf(stderr, "DEBUG: Comando 'birdc' non trovato nel PATH. Verifica l'installazione di BIRD.\n");
                *route_count = 0;
                return NULL;
        }
        
        char command[256];
        snprintf(command, sizeof(command), "sudo %s show route export %s", BIRD_COMMAND, protocol_name);
        
        FILE *fp = popen(command, "r");
        if (fp == NULL) {
                fprintf(stderr, "Errore nell'esecuzione del comando: %s\n", command);
                *route_count = 0;
                return NULL;
        }
        
        // Alloca memoria per memorizzare le rotte
        char **routes = malloc(MAX_ROUTES * sizeof(char*));
        if (routes == NULL) {
                fprintf(stderr, "Errore nell'allocazione memoria per routes\n");
                pclose(fp);
                *route_count = 0;
                return NULL;
        }
        
        char line[MAX_LINE_LENGTH];
        int count = 0;
        
        // Legge l'output del comando riga per riga
        while (fgets(line, sizeof(line), fp) && count < MAX_ROUTES) {
                // Salta righe di header e righe vuote
                if (strstr(line, "BIRD") || strstr(line, "Table") || strlen(line) == 0 || 
                    strstr(line, "ready") || line[0] == '\n') {
                        continue;
                }
                
                // Alloca memoria per la riga e la copia
                routes[count] = malloc(MAX_LINE_LENGTH);
                if (routes[count] == NULL) {
                        fprintf(stderr, "Errore nell'allocazione memoria per route\n");
                        break;
                }
                
                strncpy(routes[count], line, MAX_LINE_LENGTH - 1);
                routes[count][MAX_LINE_LENGTH - 1] = '\0';
                count++;
        }
        
        int close_result = pclose(fp);
        if (close_result != 0) {
                fprintf(stderr, "DEBUG: Comando birdc terminato con codice %d\n", close_result);
        }
        
        *route_count = count;
        return routes;
}

/**
 * Estrae gli IP dei router dalle righe di export delle rotte
 * Cerca il pattern "via X.X.X.X" nelle righe di output
 * @param routes: Array di righe di output da birdc
 * @param route_count: Numero di righe nell'array
 * @param router_ips: Array dove salvare gli IP estratti
 * @param router_count: Puntatore per restituire il numero di IP trovati
 */
void extract_router_ips(char **routes, int route_count, char **router_ips, int *router_count) {
        *router_count = 0;
        
        for (int i = 0; i < route_count && *router_count < MAX_EXPORTED_ROUTERS; i++) {
                char *line = routes[i];
                
                // Cerca il pattern "via X.X.X.X" che indica il next-hop
                char *via_pos = strstr(line, "via ");
                if (via_pos != NULL) {
                        via_pos += 4; // Salta "via "
                        
                        // Estrai l'indirizzo IP fino al primo spazio
                        char ip_buffer[MAX_IP_LENGTH] = {0};
                        int j = 0;
                        while (*via_pos && !isspace(*via_pos) && j < MAX_IP_LENGTH - 1) {
                                ip_buffer[j++] = *via_pos++;
                        }
                        ip_buffer[j] = '\0';
                        
                        // Verifica che sia un IP valido (contiene punti e numeri)
                        if (strlen(ip_buffer) > 0 && strchr(ip_buffer, '.') != NULL) {
                                // Controlla se l'IP è già nella lista
                                int found = 0;
                                for (int k = 0; k < *router_count; k++) {
                                        if (strcmp(router_ips[k], ip_buffer) == 0) {
                                                found = 1;
                                                break;
                                        }
                                }
                                
                                if (!found) {
                                        router_ips[*router_count] = malloc(MAX_IP_LENGTH);
                                        if (router_ips[*router_count] == NULL) {
                                                fprintf(stderr, "Errore allocazione memoria per router IP\n");
                                                break;
                                        }
                                        strncpy(router_ips[*router_count], ip_buffer, MAX_IP_LENGTH - 1);
                                        router_ips[*router_count][MAX_IP_LENGTH - 1] = '\0';
                                        (*router_count)++;
                                }
                        }
                }
        }
}

/**
 * Esegue birdc configure per applicare le modifiche alla configurazione
 * Necessario dopo la generazione dei nuovi filtri
 */
void execute_bird_configure() {
        if (!check_birdc_available()) {
                fprintf(stderr, "DEBUG: Comando 'birdc' non disponibile. Impossibile applicare la configurazione.\n");
                return;
        }
        
        char command[64];
        snprintf(command, sizeof(command), "sudo %s configure", BIRD_COMMAND);
        
        int result = system(command);
        
        if (result == 0) {
                printf("DEBUG: Configurazione BIRD applicata con successo!\n");
        } else {
                fprintf(stderr, "DEBUG: Errore nell'applicazione della configurazione BIRD. Codice: %d\n", result);
        }
}

// ================================
// FUNZIONI DI GENERAZIONE FILTRI
// ================================

/**
 * Genera il file di configurazione BIRD con i filtri basati su latenza
 * Crea un filter per ogni router sorgente con regole per ogni destinazione
 * @param rules: Array di regole di filtro da includere
 * @param rule_count: Numero di regole nell'array
 */
void generate_bird_filters(FilterRule *rules, int rule_count) {
        printf("DEBUG: Generazione file filtri BIRD con %d regole...\n", rule_count);
        
        FILE *file = fopen("/usr/local/etc/latency_filters.conf", "w");
        if (file == NULL) {
                fprintf(stderr, "Errore nell'apertura del file /usr/local/etc/latency_filters.conf\n");
                fprintf(stderr, "Assicurati di avere i permessi di scrittura nella directory /usr/local/etc/\n");
                return;
        }
        
        // Header del file con timestamp
        time_t now = time(NULL);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%a %d %b %Y, %H:%M:%S, %Z", localtime(&now));
        
        fprintf(file, "# Filtri BIRD generati automaticamente basati su latenza\n");
        fprintf(file, "# Generato il: %s\n\n", timestamp);
        
        // Raggruppa regole per IP sorgente (per creare un filter per ogni router)
        char unique_sources[MAX_PROTOCOLS][MAX_IP_LENGTH];
        int source_count = 0;
        
        for (int i = 0; i < rule_count; i++) {
                int found = 0;
                for (int j = 0; j < source_count; j++) {
                        if (strcmp(unique_sources[j], rules[i].source_ip) == 0) {
                                found = 1;
                                break;
                        }
                }
                if (!found) {
                        strncpy(unique_sources[source_count], rules[i].source_ip, MAX_IP_LENGTH - 1);
                        source_count++;
                }
        }
        
        printf("DEBUG: Trovati %d router unici per generazione filtri\n", source_count);
        
        // Genera un filter per ogni router sorgente
        for (int i = 0; i < source_count; i++) {
                // Crea nome filter sostituendo punti con underscore nell'IP
                char filter_name[64];
                snprintf(filter_name, sizeof(filter_name), "export_");
                
                const char *ip_ptr = unique_sources[i];
                char *name_ptr = filter_name + strlen(filter_name);
                
                while (*ip_ptr && (name_ptr - filter_name) < (int)sizeof(filter_name) - 1) {
                        if (*ip_ptr == '.') {
                                *name_ptr++ = '_';
                        } else {
                                *name_ptr++ = *ip_ptr;
                        }
                        ip_ptr++;
                }
                *name_ptr = '\0';
                
                // Inizia definizione del filter
                fprintf(file, "filter %s {\n", filter_name);
                
                int rules_for_source = 0;
                // Aggiungi tutte le regole per questo router sorgente
                for (int j = 0; j < rule_count; j++) {
                        if (strcmp(rules[j].source_ip, unique_sources[i]) == 0) {
                                fprintf(file, "    # Per rotte provenienti da %s: %.3f ms -> preference %d\n",
                                        rules[j].target_ip, rules[j].total_latency, rules[j].preference);
                                fprintf(file, "    if (from = %s) then {\n", rules[j].target_ip);
                                fprintf(file, "        bgp_local_pref = %d;\n", rules[j].preference);
                                fprintf(file, "    }\n");
                                rules_for_source++;
                        }
                }
                
                fprintf(file, "    accept;\n}\n\n");
                printf("DEBUG: Generato filter %s con %d regole\n", filter_name, rules_for_source);
        }
        
        fclose(file);
        printf("DEBUG: File di configurazione generato: /usr/local/etc/latency_filters.conf\n");
        
        // Applica la nuova configurazione a BIRD
        execute_bird_configure();
}

/**
 * Processa tutti i protocolli BGP per generare i filtri di latenza
 * Questa è la funzione principale che coordina l'intero processo di generazione filtri
 */
void process_latency_filters() {
        printf("\n=== INIZIO ELABORAZIONE FILTRI LATENZA ===\n");
        
        // Carica i dati ping in cache per accesso rapido
        int ping_data_available = load_ping_cache();
        
        if (!ping_data_available) {
                printf("DEBUG: Dati ping non disponibili. I filtri di latenza verranno generati al prossimo ciclo.\n");
                printf("=== FINE ELABORAZIONE FILTRI LATENZA (dati non disponibili) ===\n\n");
                return;
        }
        
        // Recupera tutti i protocolli BGP dal database
        const char *query = "SELECT protocol_name, peer_ip FROM bgp_connections";
        
        if (mysql_query(conn, query)) {
                fprintf(stderr, "Errore nel recupero protocolli BGP: %s\n", mysql_error(conn));
                return;
        }
        
        MYSQL_RES *result = mysql_store_result(conn);
        if (result == NULL) {
                fprintf(stderr, "Errore nel store result protocolli BGP: %s\n", mysql_error(conn));
                return;
        }
        
        int bgp_count = mysql_num_rows(result);
        printf("DEBUG: Trovati %d protocolli BGP nel database\n", bgp_count);
        
        if (bgp_count == 0) {
                printf("DEBUG: Nessun protocollo BGP trovato, skipping elaborazione filtri\n");
                mysql_free_result(result);
                return;
        }
        
        if (!check_birdc_available()) {
                fprintf(stderr, "DEBUG: Comando 'birdc' non disponibile. Impossibile generare filtri di latenza.\n");
                mysql_free_result(result);
                return;
        }
        
        // Alloca memoria per le regole di filtro
        FilterRule *filter_rules = malloc(MAX_FILTER_RULES * sizeof(FilterRule));
        if (filter_rules == NULL) {
                fprintf(stderr, "Errore nell'allocazione memoria per filter rules\n");
                mysql_free_result(result);
                return;
        }
        int rule_count = 0;
        
        // Processa ogni protocollo BGP
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
                const char *protocol_name = row[0];
                const char *peer_ip = row[1];
                
                // Ottieni le rotte esportate da questo protocollo
                int route_count = 0;
                char **routes = execute_birdc_command(protocol_name, &route_count);
                
                if (routes != NULL && route_count > 0) {
                        char **router_ips = malloc(MAX_EXPORTED_ROUTERS * sizeof(char*));
                        if (router_ips == NULL) {
                                fprintf(stderr, "Errore allocazione memoria per router_ips\n");
                                // Libera routes prima di uscire
                                for (int i = 0; i < route_count; i++) free(routes[i]);
                                free(routes);
                                continue;
                        }
                        
                        int router_count = 0;
                        extract_router_ips(routes, route_count, router_ips, &router_count);
                        
                        // Per ogni router di destinazione, calcola latenza e genera regola
                        for (int i = 0; i < router_count; i++) {
                                if (rule_count >= MAX_FILTER_RULES) {
                                        fprintf(stderr, "AVVISO: Raggiunto limite massimo regole filtro (%d)\n", MAX_FILTER_RULES);
                                        break;
                                }
                                
                                double min_latency = calculate_min_latency(peer_ip, router_ips[i]);
                                int preference = calculate_preference(min_latency);
                                
                                strncpy(filter_rules[rule_count].source_ip, peer_ip, MAX_IP_LENGTH - 1);
                                strncpy(filter_rules[rule_count].target_ip, router_ips[i], MAX_IP_LENGTH - 1);
                                filter_rules[rule_count].total_latency = min_latency;
                                filter_rules[rule_count].preference = preference;
                                rule_count++;
                                
                                free(router_ips[i]);
                        }
                        
                        free(router_ips);
                        
                        // Libera memoria delle rotte
                        for (int i = 0; i < route_count; i++) {
                                free(routes[i]);
                        }
                        free(routes);
                }
        }
        
        mysql_free_result(result);
        
        printf("DEBUG: Elaborazione completata. Generate %d regole di filtro\n", rule_count);
        
        if (rule_count > 0) {
                generate_bird_filters(filter_rules, rule_count);
        } else {
                printf("DEBUG: Nessuna regola generata, file di configurazione non creato\n");
        }
        
        free(filter_rules);
        
        // Libera la cache ping
        if (ping_cache != NULL) {
                free(ping_cache);
                ping_cache = NULL;
                ping_cache_size = 0;
                ping_cache_capacity = 0;
        }
        
        printf("=== FINE ELABORAZIONE FILTRI LATENZA ===\n\n");
}

// ================================
// FUNZIONI DI PARSING OUTPUT BIRD
// ================================

/**
 * Funzione helper per salvare un protocollo se valido
 * Controlla tutti i criteri necessari per considerare un protocollo valido
 */
void save_protocol_if_valid(ProtocolInfo *proto, ProtocolInfo *active_protocols, int *active_count, int *processed_count) {
        if (proto->is_bgp && 
            strlen(proto->name) > 0 && strlen(proto->peer_ip) > 0 &&
            proto->is_up && proto->bgp_established) {
                
                insert_protocol_data(proto->name, proto->peer_ip);
                
                if (*active_count < MAX_PROTOCOLS) {
                        memcpy(&active_protocols[*active_count], proto, sizeof(ProtocolInfo));
                        (*active_count)++;
                }
                
                (*processed_count)++;
        }
}

/**
 * Funzione principale che analizza l'output di "birdc show protocols all"
 * Legge dallo standard input e identifica le sessioni BGP stabilite
 * Gestisce il salvataggio nel database e la pulizia delle sessioni obsolete
 */
void parse_birdc_output() {
        char line[MAX_LINE_LENGTH];
        ProtocolInfo current_protocol = {0};
        ProtocolInfo active_protocols[MAX_PROTOCOLS];
        int active_count = 0;
        int in_protocol_section = 0;
        int protocols_processed = 0;
        
        // Legge l'output di birdc riga per riga dallo stdin
        while (fgets(line, sizeof(line), stdin)) {
                line[strcspn(line, "\n")] = 0;
                
                // Salta righe di header e righe vuote
                if (strlen(line) == 0 || strstr(line, "BIRD") || strstr(line, "Name       Proto")) {
                        continue;
                }
                
                // Riga che inizia con nome protocollo (non indentata) - inizio nuova sezione
                if (strlen(line) > 0 && line[0] != ' ' && strchr(line, ' ') != NULL) {
                        // Salva il protocollo precedente se valido
                        if (in_protocol_section) {
                                save_protocol_if_valid(&current_protocol, active_protocols, &active_count, &protocols_processed);
                        }
                        
                        // Inizializza nuovo protocollo
                        memset(&current_protocol, 0, sizeof(current_protocol));
                        in_protocol_section = 1;
                        current_protocol.bgp_established = 0;
                        
                        // Parsa i campi principali della riga
                        char name[100], proto[50], table[50], state[50], since[50], info[100];
                        if (sscanf(line, "%s %s %s %s %s %[^\n]", 
                                   name, proto, table, state, since, info) >= 5) {
                                strncpy(current_protocol.name, name, sizeof(current_protocol.name) - 1);
                                strncpy(current_protocol.proto_type, proto, sizeof(current_protocol.proto_type) - 1);
                                strncpy(current_protocol.state, state, sizeof(current_protocol.state) - 1);
                                current_protocol.is_bgp = (strcmp(proto, "BGP") == 0);
                                current_protocol.is_up = (strcasecmp(state, "up") == 0);
                        }
                        continue;
                }
                
                // Righe indentate - dettagli del protocollo corrente
                if (in_protocol_section && current_protocol.is_bgp) {
                        if (strstr(line, "Neighbor address:")) {
                                // Estrai l'IP del peer BGP
                                char *ip_start = strstr(line, ":");
                                if (ip_start) {
                                        ip_start += 1;
                                        while (*ip_start == ' ') ip_start++;
                                        strncpy(current_protocol.peer_ip, ip_start, MAX_IP_LENGTH - 1);
                                        current_protocol.peer_ip[MAX_IP_LENGTH - 1] = '\0';
                                }
                        }
                        
                        if (strstr(line, "BGP state:")) {
                                // Verifica se la sessione BGP è in stato Established
                                current_protocol.bgp_established = strstr(line, "Established") != NULL;
                        }
                        
                        if (strstr(line, "State:") && strstr(line, "UP")) {
                                current_protocol.is_up = 1;
                        }
                }
        }
        
        // Salva l'ultimo protocollo processato
        if (in_protocol_section) {
                save_protocol_if_valid(&current_protocol, active_protocols, &active_count, &protocols_processed);
        }
        
        // Pulizia protocolli obsoleti dal database
        cleanup_old_protocols(active_protocols, active_count);
        
        printf("Elaborazione completata. Protocolli BGP Established processati: %d, Attivi: %d\n", 
               protocols_processed, active_count);
        
        // Avvia la generazione dei filtri di latenza
        process_latency_filters();
}

// ================================
// FUNZIONE MAIN
// ================================

/**
 * Funzione principale del programma
 * Gestisce l'inizializzazione e coordina l'esecuzione delle varie fasi
 */
int main(int argc, char *argv[]) {
        printf("=== PROGRAMMA C - FILTRO BGP ESTABLISHED E GENERATORE FILTRI LATENZA ===\n");
        
        // Verifica parametri di input
        if (argc != 5) {
                fprintf(stderr, "Uso: %s <db_host> <db_user> <db_password> <db_name>\n", argv[0]);
                return 1;
        }
        
        const char *db_host = argv[1];
        const char *db_user = argv[2];
        const char *db_password = argv[3];
        const char *db_name = argv[4];
        
        // Inizializza connessione database
        init_database(db_host, db_user, db_password, db_name);
        
        // Elabora l'output di birdc dallo standard input
        parse_birdc_output();
        
        // Chiude connessione database
        mysql_close(conn);
        printf("=== PROGRAMMA C - COMPLETATO ===\n");
        return 0;
}
