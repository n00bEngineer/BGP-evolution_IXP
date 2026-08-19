//gcc -o inconsistent_filter inconsis_filter_stat.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINE 1024

typedef struct {
    char ip[46]; // 45 per IPv6 + null
    int is_ipv6;
} ip_entry;

// Buffer circolare per memorizzare gli IP trovati
ip_entry *ip_list = NULL;
size_t ip_count = 0;
size_t ip_capacity = 0;

// Funzione ottimizzata per estrarre IP da righe neighbor
int extract_ip(const char *line, char *ip_buf, int *is_ipv6) {
    const char *neighbor = strstr(line, "neighbor");
    if (!neighbor) return 0;
    
    const char *ip_start = NULL;
    const char *ip_end = NULL;
    const char *ptr = neighbor + 8; // "neighbor"
    
    // Salta spazi
    while (*ptr && (*ptr == ' ' || *ptr == '\t')) ptr++;
    if (!*ptr) return 0;
    
    ip_start = ptr;
    
    // Determina se è IPv4 o IPv6 e trova la fine
    if (strchr(ip_start, ':')) {
        // IPv6
        *is_ipv6 = 1;
        ip_end = ip_start;
        while (*ip_end && !isspace(*ip_end)) ip_end++;
    } else {
        // IPv4
        *is_ipv6 = 0;
        ip_end = ip_start;
        while (*ip_end && (*ip_end == '.' || isdigit(*ip_end))) ip_end++;
    }
    
    if (ip_end > ip_start && (ip_end - ip_start) < 45) {
        strncpy(ip_buf, ip_start, ip_end - ip_start);
        ip_buf[ip_end - ip_start] = '\0';
        return 1;
    }
    
    return 0;
}

// Aggiunge IP alla lista evitando duplicati
void add_ip(const char *ip, int is_ipv6) {
    // Controlla duplicati
    for (size_t i = 0; i < ip_count; i++) {
        if (strcmp(ip_list[i].ip, ip) == 0) {
            return;
        }
    }
    
    // Ridimensiona se necessario
    if (ip_count >= ip_capacity) {
        ip_capacity = ip_capacity ? ip_capacity * 2 : 100;
        ip_list = realloc(ip_list, ip_capacity * sizeof(ip_entry));
    }
    
    strcpy(ip_list[ip_count].ip, ip);
    ip_list[ip_count].is_ipv6 = is_ipv6;
    ip_count++;
}

// Converte IP in nome filtro
void make_filter_name(const char *ip, int is_ipv6, char *filter_name) {
    strcpy(filter_name, "export_");
    char *dest = filter_name + 7;
    
    if (!is_ipv6) {
        // IPv4: sostituisce punti con underscore
        for (const char *src = ip; *src && dest < filter_name + 95; src++) {
            *dest++ = (*src == '.') ? '_' : *src;
        }
    } else {
        // IPv6: sostituisce ':' con '_'
        for (const char *src = ip; *src && dest < filter_name + 95; src++) {
            *dest++ = (*src == ':') ? '_' : *src;
        }
    }
    *dest = '\0';
}

int main() {
    FILE *input_file = fopen("/usr/local/etc/bird.conf", "r");
    if (!input_file) {
        perror("Errore apertura file bird.conf");
        return 1;
    }
    
    FILE *output_file = fopen("/usr/local/etc/latency_filters.conf", "w");
    if (!output_file) {
        perror("Errore creazione file latency_filters.conf");
        fclose(input_file);
        return 1;
    }
    
    char line[MAX_LINE];
    size_t line_count = 0;
    
    // Prima passata: conta righe per stima (opzionale)
    while (fgets(line, sizeof(line), input_file)) line_count++;
    rewind(input_file);
    
    fprintf(output_file, "# Filtri di export generati automaticamente per l'avvio di BIRD con latency preference i protocolli BGP\n\n");
    
    // Seconda passata: elaborazione
    while (fgets(line, sizeof(line), input_file)) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        
        // Salta rapidamente commenti e righe vuote
        if (*trimmed == '#' || *trimmed == '\n' || *trimmed == '\r') continue;
        
        // Processa solo righe con 'neighbor'
        if (strstr(trimmed, "neighbor")) {
            char ip[46];
            int is_ipv6;
            
            if (extract_ip(trimmed, ip, &is_ipv6)) {
                add_ip(ip, is_ipv6);
            }
        }
    }
    
    fclose(input_file);
    
    // Genera output nel file
    for (size_t i = 0; i < ip_count; i++) {
        char filter_name[100];
        make_filter_name(ip_list[i].ip, ip_list[i].is_ipv6, filter_name);
        
        fprintf(output_file, "filter %s {\n", filter_name);
        fprintf(output_file, "    accept;\n");
        fprintf(output_file, "}\n\n");
    }
    
    // Cleanup
    fclose(output_file);
    free(ip_list);
    
    printf("File iniziale latency_filters.conf generato con successo in /usr/local/etc/\n");
    printf("Trovati %zu indirizzi IP\n", ip_count);
    
    return 0;
}
