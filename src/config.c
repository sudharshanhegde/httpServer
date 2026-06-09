#include "server.h"

static char *trim_whitespace(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')
    {
        str++;
    }
    if(*str == '\0')
    {
        return str;
    }
    end = str + strlen(str) - 1;
    while(end > str && (*end == ' ' || end == '\t' || end == '\r' || end == '\n'))
    {
        end--;
    }
    *(end + 1) = '\0';
    return str;
}

void config_load(const char *filename, struct server_config *cfg) {
    FILE *file;
    char line[CONFIG_LINE_SIZE];
    char *key = NULL;
    char *value = NULL;
    int line_num = 0;
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->server_root, "./www", sizeof(cfg->server_root) - 1);
    cfg->port = DEFAULT_PORT;
    cfg->num_threads = DEFAULT_THREADS;
    strncpy(cfg->log_file, "./logs/server.log", sizeof(cfg->log_file) - 1);
    cfg->timeout_secs = 30;
    file = fopen(filename, "r");
    if(!file) {
        fprintf(stderr, "could not open the config file '%s' using the default values\n",filename);
        return;
    }
    while (fgets(line,sizeof(line),file))
    {
        line_num++;
        size_t len = strlen(line);
        if(len > 0 && line[len-1] == '\n')
        {
            line[len-1] = '\0';
        }
        char *trimmed = trim_whitespace(line);
        if(trimmed[0] == '\0' || trimmed[0] == '#')
        {
            continue;
        }
        key = strtok(trimmed, "=");
        value = strtok(NULL,"=");
        if(!key || !value)
        {
            continue;
        }
        key = trim_whitespace(key);
        value = trim_whitespace(value);
        if(strncmp(key,"port",sizeof("port")) == 0)
        {
            cfg->port = atoi(value);
            if(cfg->port < 1 || cfg->port > 65535){
                fprintf(stderr, "Invalid port %s has been used use the values between 1 and 65535 please",value);
                printf("Falling back to the default port %d",DEFAULT_PORT);
                cfg->port = DEFAULT_PORT;
            }
        }
        else if (strncmp(key, "server_root",sizeof("server_root")) == 0) {
            strncpy(cfg->server_root, value, sizeof(cfg->server_root)-1);
            cfg->server_root[sizeof(cfg->server_root) - 1] = '\0';
        } 
        else if(strncmp(key, "num_threads", sizeof("num_threads")) == 0)
        {
            cfg -> num_threads = atoi(value);
            if(cfg->num_threads < 1 || cfg->num_threads > 128)
            {
                fprintf(stderr, "Invalid num_threads %s has been used", value);
                printf("Setting default value of %d", DEFAULT_THREADS);
                cfg->num_threads = DEFAULT_THREADS;
            }
        }
        else if(strncmp(key, "log_file", sizeof("log_file")) == 0)
        {
            strncpy(cfg->log_file, value, sizeof(cfg->log_file) - 1);
            cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
        }
        else if (strncmp(key, "timeout_secs", sizeof("timeout_secs")) == 0)
        {
            cfg->timeout_secs = atoi(value);
            if(cfg->timeout_secs < 1)
            {
                fprintf(stderr, "Invalid timeout_secs %s at line %d so will be using default value of '30'\n",value,line_num);
                cfg->timeout_secs = 30;
            }
        }
        else{
            fprintf(stderr, "unknown config key %s at line %d, so we are ignoring it\n", key, line_num);
        }
    }
    fclose(file);
}

void config_print(const struct server_config *cfg)
{
    fprintf(stdout, "Server configuration is as follows : ");
    fprintf(stdout, "Port number is : %d\n", cfg->port);
    fprintf(stdout, "server root is : %s", cfg->server_root);
    fprintf(stdout, "Threads number is : %d\n", cfg->num_threads);
    fprintf(stdout,"log file : %s\n", cfg->log_file);
    fprintf(stdout,"timeout period is : %d seconds", cfg->timeout_secs);
    fprintf(stdout, "Done this is all we have\n");
}