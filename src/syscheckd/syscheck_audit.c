/*
 * Copyright (C) 2018 Wazuh Inc.
 * June 13, 2018.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "shared.h"
#include "external/procps/readproc.h"
#include <linux/audit.h>
#include <libaudit.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "syscheck.h"

#define AUDIT_CONF_FILE "/etc/audisp/plugins.d/af_wazuh.conf"
#define AUDIT_SOCKET DEFAULTDIR "/queue/ossec/audit"
#define BUF_SIZE 4096

// Check if auditd is installed and running
int check_auditd_enabled(void) {

    PROCTAB *proc = openproc(PROC_FILLSTAT | PROC_FILLSTATUS | PROC_FILLCOM );
    proc_t *proc_info;
    int auditd_pid = -1;

    while (proc_info = readproc(proc, NULL), proc_info != NULL) {
        if(strcmp(proc_info->cmd,"auditd") == 0) {
            auditd_pid = proc_info->tid;
            break;
        }
    }

    freeproc(proc_info);

    return auditd_pid;
}

// Set audit socket configuration
int set_auditd_config(void) {

    if (!IsFile(AUDIT_CONF_FILE)) {
        return 0;
    }

    minfo("Generating Auditd socket configuration file: %s", AUDIT_CONF_FILE);

    FILE *fp;
    fp = fopen(AUDIT_CONF_FILE, "w");
    if (!fp) return -1;

    fprintf(fp, "active = yes\n");
    fprintf(fp, "direction = out\n");
    fprintf(fp, "path = builtin_af_unix\n");
    fprintf(fp, "type = builtin\n");
    fprintf(fp, "args = 0640 %s\n", AUDIT_SOCKET);
    fprintf(fp, "format = string\n");
    fclose(fp);

    mwarn("Auditsp configuration was modified. You need to restart Auditd. Who-data will be disabled.");
    return 1;
}


// Init audit socket
int init_auditd_socket(void) {

    int sfd;
    struct sockaddr_un addr;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return (-1);
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AUDIT_SOCKET, sizeof(addr.sun_path)-1);

    if (connect(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) < 0) {
        mdebug1("Cannot connect to socket %s\n", AUDIT_SOCKET);
        close(sfd);
        return (-1);
    }

    return sfd;
}


// Add / delete rules
int audit_manage_rules(int action, const char *path, const char *key) {

    int retval, output;
    int type;
    struct stat buf;
    int audit_handler;

    audit_handler = audit_open();
    if (audit_handler < 0) {
        return (-1);
    }

    struct audit_rule_data *myrule = NULL;
    myrule = malloc(sizeof(struct audit_rule_data));
    memset(myrule, 0, sizeof(struct audit_rule_data));

    // Check path
    if (stat(path, &buf) == 0) {
        if (S_ISDIR(buf.st_mode)){
            type = AUDIT_DIR;
        }
        else {
            type = AUDIT_WATCH;
        }
    } else {
        merror("audit_manage_rules(): Cannot stat %s", path);
        retval = -1;
        goto end;
    }

    // Set watcher
    output = audit_add_watch_dir(type, &myrule, path);
    if (output) {
        mdebug2("audit_add_watch_dir = (%d) %s", output, audit_errno_to_name(abs(output)));
        retval = -1;
        goto end;
    }

    // Set permisions
    int permisions = 0;
    permisions |= AUDIT_PERM_WRITE;
    permisions |= AUDIT_PERM_ATTR;
    output = audit_update_watch_perms(myrule, permisions);
    if (output) {
        mdebug2("audit_update_watch_perms = (%d) %s", output, audit_errno_to_name(abs(output)));
        retval = -1;
        goto end;
    }

    // Set key
    int flags = AUDIT_FILTER_EXIT & AUDIT_FILTER_MASK;

    if (strlen(key) > (AUDIT_MAX_KEY_LEN - 5)) {
        retval = -1;
        goto end;
    }

    char *cmd = malloc(sizeof(char) * AUDIT_MAX_KEY_LEN + 1);

    if (snprintf(cmd, AUDIT_MAX_KEY_LEN, "key=%s", key) < 0) {
        free(cmd);
        retval = -1;
        goto end;
    } else {
        output = audit_rule_fieldpair_data(&myrule, cmd, flags);
        if (output) {
            mdebug2("audit_rule_fieldpair_data = (%d) %s", output, audit_errno_to_name(abs(output)));
            free(cmd);
            retval = -1;
            goto end;
        }
        free(cmd);
    }

    // Add/Delete rule
    if (action == ADD_RULE) {
        retval = abs(audit_add_rule_data(audit_handler, myrule, flags, AUDIT_ALWAYS));
    } else if (action == DELETE_RULE){
        retval = abs(audit_delete_rule_data(audit_handler, myrule, flags, AUDIT_ALWAYS));
    } else {
        retval = -1;
        goto end;
    }

    if (retval != 1) {
        mdebug2("audit_manage_rules(): Error adding/deleting rule (%d) = %s", retval, audit_errno_to_name(retval));
    }

end:
    audit_rule_free_data(myrule);
    audit_close(audit_handler);
    return retval;
}


int audit_init(void) {

    // Check if auditd is installed and running.
    int aupid = check_auditd_enabled();
    if (aupid <= 0) {
        mdebug1("Auditd is not running.");
        return (-1);
    }

    // Check audit socket configuration
    switch (set_auditd_config()) {
    case -1:
        mdebug1("Cannot generate Audit config.");
        return (-1);
    case 0:
        break;
    default:
        return (-1);
    }

    return init_auditd_socket();
}


void * audit_main(int * audit_sock) {

    regex_t regexCompiled_uid;
    regex_t regexCompiled_pid;
    regex_t regexCompiled_pname;
    regex_t regexCompiled_path;
    regmatch_t match[2];
    int match_size;
    char *uid = NULL;
    char *pid = NULL;
    char *pname = NULL;
    char *path = NULL;
    int byteRead = 0;

    whodata_evt *w_evt;
    os_calloc(1, sizeof(whodata_evt), w_evt);

    char *buffer;
    buffer = malloc(BUF_SIZE * sizeof(char));

    static const char *pattern_uid = " uid=([0-9]*) ";
    if (regcomp(&regexCompiled_uid, pattern_uid, REG_EXTENDED)) {
        merror("Cannot compile uid regular expression.");
    }
    static const char *pattern_pid = " pid=([0-9]*) ";
    if (regcomp(&regexCompiled_pid, pattern_pid, REG_EXTENDED)) {
        merror("Cannot compile pid regular expression.");
    }
    static const char *pattern_pname = " exe=\"([^ ]*)\" ";
    if (regcomp(&regexCompiled_pname, pattern_pname, REG_EXTENDED)) {
        merror("Cannot compile pname regular expression.");
    }
    static const char *pattern_path = " item=1 name=\"([^ ]*)\" ";
    if (regcomp(&regexCompiled_path, pattern_path, REG_EXTENDED)) {
        merror("Cannot compile path regular expression.");
    }

    mdebug1("Reading events from Audit socket ...");

    while ((byteRead = recv(*audit_sock, buffer, BUF_SIZE, 0)) > 0) {

        buffer[byteRead] = '\0';
        char *ret;
        if (ret = strstr(buffer,"key=\"wazuh_fim\""), ret) {

            if(regexec(&regexCompiled_uid, buffer, 2, match, 0) == 0) {
                match_size = match[1].rm_eo - match[1].rm_so;
                uid = malloc(match_size + 1);
                snprintf (uid, match_size +1, "%.*s", match_size, buffer + match[1].rm_so);
                w_evt->user_name = (char *)get_user("",atoi(uid));
                w_evt->user_id = strdup(uid);
                free(uid);
            }

            if(regexec(&regexCompiled_pid, buffer, 2, match, 0) == 0) {
                match_size = match[1].rm_eo - match[1].rm_so;
                pid = malloc(match_size + 1);
                snprintf (pid, match_size +1, "%.*s", match_size, buffer + match[1].rm_so);
                w_evt->process_id = atoi(pid);
                free(pid);
            }

            if(regexec(&regexCompiled_path, buffer, 2, match, 0) == 0) {
                match_size = match[1].rm_eo - match[1].rm_so;
                path = malloc(match_size + 1);
                snprintf (path, match_size +1, "%.*s", match_size, buffer + match[1].rm_so);
                w_evt->path = strdup(path);
                free(path);
            }

            if(regexec(&regexCompiled_pname, buffer, 2, match, 0) == 0) {
                match_size = match[1].rm_eo - match[1].rm_so;
                pname = malloc(match_size + 1);
                snprintf (pname, match_size +1, "%.*s", match_size, buffer + match[1].rm_so);
                w_evt->process_name = strdup(pname);
                free(pname);
            }

            if (w_evt->path) {
                mdebug1("audit_event: uid=%s, pid=%i, path=%s, pname=%s", w_evt->user_name, w_evt->process_id, w_evt->path, w_evt->process_name);
                realtime_checksumfile(w_evt->path, w_evt);
            }
        }
    }

    // Auditd is not runnig or socket closed.
    merror("Audit thread finished.");
    free(buffer);
    close(*audit_sock);
    regfree(&regexCompiled_uid);
    regfree(&regexCompiled_pid);
    regfree(&regexCompiled_path);
    regfree(&regexCompiled_pname);

    return NULL;
}


// Add rule to kernel
int add_audit_rule(const char *dir) {
    if (dir)
        return 1;
    else return 0;
}