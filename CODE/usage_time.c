#include "usage_time.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../hashmap.c/hashmap.h"

#define PATH_BUF_SIZE 32
#define NAME_BUF_SIZE 32

#define MAX_TOKENS 4
#define MAX_TOKEN_LENGTH 100
#define MAX_LINE_LENGTH 100

int mday;
struct hashmap* usage_time;
struct hashmap* curr;
struct hashmap* prev;
struct hashmap* exclude;
mqd_t mq;

int record_compare(const void* a, const void* b, void* rdata) {
    const name_time* ra = a;
    const name_time* rb = b;
    return strcmp(ra->name, rb->name);
}

bool start_time_iter(const void* item, void* rdata) {
    const name_time* start_time_ptr = item;
    struct tm* start_time_tm_ptr = localtime(&(start_time_ptr->time));
    printf("%s, started at %d/%d/%d %d:%d:%d\n",
           start_time_ptr->name,
           start_time_tm_ptr->tm_year + 1900,
           start_time_tm_ptr->tm_mon + 1,
           start_time_tm_ptr->tm_mday,
           start_time_tm_ptr->tm_hour,
           start_time_tm_ptr->tm_min,
           start_time_tm_ptr->tm_sec);
    return true;
}

bool usage_time_iter(const void* item, void* rdata) {
    const name_time* usage_time_ptr = item;
    struct tm* usage_time_tm_ptr = localtime(&(usage_time_ptr->time));
    printf("%s, used for %d:%d:%d\n",
           usage_time_ptr->name,
           (usage_time_tm_ptr->tm_mday - 1) * 24 + usage_time_tm_ptr->tm_hour - 9,
           usage_time_tm_ptr->tm_min,
           usage_time_tm_ptr->tm_sec);
    return true;
}

uint64_t record_hash(const void* item, uint64_t seed0, uint64_t seed1) {
    const name_time* record_ptr = item;
    return hashmap_sip(record_ptr->name, strlen(record_ptr->name), seed0, seed1);
}

void setup_map() {
    usage_time = hashmap_new(sizeof(name_time), 0, 0, 0, record_hash, record_compare, NULL, NULL);
    curr = hashmap_new(sizeof(name_time), 0, 0, 0, record_hash, record_compare, NULL, NULL);
    prev = hashmap_new(sizeof(name_time), 0, 0, 0, record_hash, record_compare, NULL, NULL);
    exclude = hashmap_new(sizeof(name_time), 0, 0, 0, record_hash, record_compare, NULL, NULL);
    read_map_from_file(exclude, "exclude_process.log");

    mq = -1;
}

void cleanup_map() {
    hashmap_free(usage_time);
    hashmap_free(curr);
    hashmap_free(prev);
    hashmap_free(exclude);
    puts("\ngoodbye...");
    exit(EXIT_SUCCESS);
}

bool is_number_string(char* str) {
    int i = 0;
    while (str[i] != '\0') {
        if (!isdigit(str[i])) return false;
        i++;
    }
    return true;
}

bool is_user_process(struct stat* stat_ptr, char* pid_str, int uid) {
    char pid_path[PATH_BUF_SIZE];
    sprintf(pid_path, "/proc/%s", pid_str);

    stat(pid_path, stat_ptr);

    return stat_ptr->st_uid == uid ? true : false;
}

void get_process_name_by_pid_string(char* buf_ptr, char* pid_str) {
    char status_path[PATH_BUF_SIZE];
    sprintf(status_path, "/proc/%s/status", pid_str);

    FILE* fp = fopen(status_path, "r");
    fscanf(fp, "%s", buf_ptr);
    fscanf(fp, "%s", buf_ptr);

    fclose(fp);
}

void get_user_process() {
    DIR* dir_ptr = opendir("/proc");
    if (dir_ptr == NULL) {
        perror("opendir");
        exit(1);
    }

    struct dirent* dirent_ptr;
    struct stat stat_buf;
    uid_t uid = geteuid();
    name_time curr_buf;
    name_time* curr_ptr;

    while ((dirent_ptr = readdir(dir_ptr)) != NULL) {
        if (is_number_string(dirent_ptr->d_name)) {
            if (is_user_process(&stat_buf, dirent_ptr->d_name, uid)) {
                curr_buf.time = stat_buf.st_ctime;
                get_process_name_by_pid_string(curr_buf.name, dirent_ptr->d_name);

                curr_ptr = hashmap_get(curr, &curr_buf);

                // 한 프로그램에 대해 여러 개의 프로세스가 실행된 경우, 더 일찍 실행된 프로세스를 선택함
                if (curr_ptr) {
                    if (curr_buf.time < curr_ptr->time) {
                        hashmap_delete(curr, curr_ptr);
                        hashmap_set(curr, &curr_buf);
                    }
                } else {
                    hashmap_set(curr, &curr_buf);
                }
            }
        }
    }

    closedir(dir_ptr);
}

void write_map_to_file(struct hashmap* map, char* filename) {
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }

    size_t iter = 0;
    void* item;
    while (hashmap_iter(map, &iter, &item)) {
        const name_time* record_ptr = item;
        fprintf(fp, "%s %ld\n", record_ptr->name, record_ptr->time);
    }

    fclose(fp);
}

void read_map_from_file(struct hashmap* map, char* filename) {
    // 최초 실행 시, 로그 파일이 존재하지 않을 수 있음
    FILE* fp = fopen(filename, "r");
    if (fp == NULL) return;

    name_time record_buf = {.time = 0, .name = ""};
    while (!feof(fp)) {
        fscanf(fp, "%s %ld\n", record_buf.name, &record_buf.time);
        hashmap_set(map, &record_buf);
    }

    fclose(fp);
}

void write_access_time_to_file(time_t access_time) {
    FILE* fp = fopen("access_time.log", "w");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }

    fprintf(fp, "%ld\n", access_time);
    fclose(fp);
}

time_t read_access_time_from_file() {
    time_t access_time;

    FILE* fp = fopen("access_time.log", "r");
    if (fp == NULL) return -1;  // 최초 실행 시

    fscanf(fp, "%ld\n", &access_time);
    fclose(fp);

    return access_time;
}

time_t get_start_time_today(time_t start_time, time_t now) {
    struct tm* start_time_tm_ptr = localtime(&start_time);
    int start_time_mday = start_time_tm_ptr->tm_mday;

    struct tm* now_tm_ptr = localtime(&now);
    int now_mday = now_tm_ptr->tm_mday;

    if (start_time_mday == now_mday)
        return start_time;

    now_tm_ptr->tm_sec = 0;
    now_tm_ptr->tm_min = 0;
    now_tm_ptr->tm_hour = 0;

    time_t start_time_today = mktime(now_tm_ptr);

    return start_time_today;
}

void swap_curr_prev() {
    struct hashmap* tmp;
    tmp = curr;
    curr = prev;
    prev = tmp;
}

void read_usage_time_from_file() {
    time_t now = time(NULL);
    struct tm* now_tm_ptr = localtime(&now);
    mday = now_tm_ptr->tm_mday;

    char filename[NAME_BUF_SIZE];
    sprintf(filename, "usage_time_%d.log", now_tm_ptr->tm_wday);
    read_map_from_file(usage_time, filename);

    time_t access_time = read_access_time_from_file();

    get_user_process();

    size_t iter = 0;
    void* item;
    while (hashmap_iter(curr, &iter, &item)) {
        const name_time* start_time_ptr = item;
        const name_time* usage_time_ptr = hashmap_get(usage_time, start_time_ptr);
        name_time usage_time_buf;

        strcpy(usage_time_buf.name, start_time_ptr->name);
        usage_time_buf.time = usage_time_ptr ? usage_time_ptr->time : 0;

        if (start_time_ptr->time < access_time)
            usage_time_buf.time += now - get_start_time_today(access_time, now);
        else
            usage_time_buf.time += now - get_start_time_today(start_time_ptr->time, now);

        hashmap_set(usage_time, &usage_time_buf);
    }

    write_map_to_file(usage_time, filename);
    write_access_time_to_file(now);

    swap_curr_prev();
    hashmap_clear(curr, false);
}

void write_usage_time_to_file() {
    time_t now = time(NULL);
    struct tm* now_tm_ptr = localtime(&now);
    if (mday != now_tm_ptr->tm_mday) {
        mday = now_tm_ptr->tm_mday;
        hashmap_clear(usage_time, false);
    }

    char filename[NAME_BUF_SIZE];
    sprintf(filename, "usage_time_%d.log", now_tm_ptr->tm_wday);

    time_t access_time = read_access_time_from_file();
    if (access_time == -1) access_time = now;

    get_user_process();

    size_t iter = 0;
    void* item;
    while (hashmap_iter(curr, &iter, &item)) {
        const name_time* start_time_ptr = item;
        const name_time* usage_time_ptr = hashmap_get(usage_time, start_time_ptr);
        name_time usage_time_buf;

        strcpy(usage_time_buf.name, start_time_ptr->name);
        usage_time_buf.time = usage_time_ptr ? usage_time_ptr->time : 0;

        if (hashmap_get(prev, start_time_ptr)) {
            usage_time_buf.time += now - get_start_time_today(access_time, now);
        } else
            usage_time_buf.time += now - get_start_time_today(start_time_ptr->time, now);

        hashmap_set(usage_time, &usage_time_buf);
    }

    iter = 0;
    while (hashmap_iter(exclude, &iter, &item)) {
        const name_time* exclude_ptr = item;
        if (hashmap_get(usage_time, exclude_ptr))
            hashmap_delete(usage_time, (void*)exclude_ptr);
    }

    write_map_to_file(usage_time, filename);
    write_access_time_to_file(now);

    swap_curr_prev();
    hashmap_clear(curr, false);
}

void add_exclude_process_by_name(char* procname) {
    name_time exclude_buf;
    memset(&exclude_buf, 0, sizeof(name_time));
    strcpy(exclude_buf.name, procname);
    hashmap_set(exclude, &exclude_buf);
    write_map_to_file(exclude, "exclude_process.log");
}

void exclude_process() {
    if (mq == -1) {
        mq = mq_open("/exclude_mq", O_RDONLY | O_NONBLOCK);
        if (mq == -1) return;
    }

    char procname[NAME_BUF_SIZE];
    int result = mq_receive(mq, procname, NAME_BUF_SIZE, NULL);
    if (result == -1) return;

    add_exclude_process_by_name(procname);
}

void execute_recover(){ // 실행 권한 복구
	FILE* fp = fopen("execute_remove.log","r");
	if(fp == NULL){
		perror("Error opening file");
		return;
	}

	fseek(fp,0,SEEK_END);
	long file_size = ftell(fp);
	if(file_size == 0){ // case that file information is zero
		printf("NO executable file to recover\n");
		fclose(fp);
		return;
	}
	
	rewind(fp); // file pointer go to first

	char line[MAX_LINE_LENGTH];
	struct stat file_stat;

	while(fgets(line,sizeof(line),fp)){
		line[strcspn(line,"\n")] = '\0'; // remove the newline character

		if(stat(line,&file_stat)==0){
			mode_t current_permissions =file_stat.st_mode;

			current_permissions |= S_IXUSR | S_IXGRP | S_IXOTH; 

			chmod(line, current_permissions); // 실행권한 복구
			time_t timer = time(NULL);
			struct tm* t = localtime(&timer);
			printf("%d/%d/%d %d:%d:%d\tpermission is recovered : %s\n",
					t->tm_hour+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,line);
		}
		else{
			perror("stat");
			break;
		}
	}

	fclose(fp);
	// 파일 권한 복구 완료했으므로 파일 내용 삭제
	FILE* file = fopen("execute_remove.log", "w"); // 파일을 쓰기 모드로 열기 (기존 내용 삭제)
    	if (file != NULL) {
        	fclose(file); // 파일 닫기
    	}
	else{
		perror("Error opening file");
	}

	// refresh();
}
