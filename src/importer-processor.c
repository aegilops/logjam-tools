#include "importer-common.h"
#include "importer-increments.h"
#include "importer-parser.h"
#include "importer-processor.h"
#include "importer-livestream.h"
#include "logjam-streaminfo.h"
#include "importer-resources.h"
#include "importer-prometheus-client.h"

#define DB_PREFIX "logjam-"
#define DB_PREFIX_LEN 7

processor_state_t* processor_new(stream_info_t *stream_info, char *db_name)
{
    processor_state_t *p = zmalloc(sizeof(processor_state_t));
    p->stream_info = stream_info;
    p->db_name = strdup(db_name);
    p->request_count = 0;
    p->modules = zhash_new();
    p->totals = zhash_new();
    p->minutes = zhash_new();
    p->quants = zhash_new();
    p->agents = zhash_new();
    p->histograms = zhash_new();
    return p;
}

void processor_destroy(void* processor)
{
    processor_state_t* p = processor;
    // printf("[D] destroying processor: %s. requests: %zu\n", p->db_name, p->request_count);
    release_stream_info(p->stream_info);
    free(p->db_name);
    zhash_destroy(&p->modules);
    zhash_destroy(&p->totals);
    zhash_destroy(&p->minutes);
    zhash_destroy(&p->quants);
    zhash_destroy(&p->agents);
    zhash_destroy(&p->histograms);
    free(p);
}

static
void dump_modules_hash(zhash_t *modules)
{
    const char* module = zhash_first(modules);
    while (module) {
        printf("[D] module: %s\n", module);
        module = zhash_next(modules);
    }
}

static
void dump_increments_hash(zhash_t *increments_hash)
{
    increments_t *increments = zhash_first(increments_hash);
    while (increments) {
        const char *action = zhash_cursor(increments_hash);
        dump_increments(action, increments);
        increments = zhash_next(increments_hash);
    }
}

static
void processor_dump_state(processor_state_t *self)
{
    puts("[D] ================================================");
    printf("[D] db_name: %s\n", self->db_name);
    printf("[D] processed requests: %zu\n", self->request_count);
    dump_modules_hash(self->modules);
    dump_increments_hash(self->totals);
    dump_increments_hash(self->minutes);
}


static
const char* append_to_json_string(json_object **jobj, const char* old_str, const char* add_str)
{
    int old_len = strlen(old_str);
    int add_len = strlen(add_str);
    int new_len = old_len + add_len;
    char new_str_value[new_len+1];
    memcpy(new_str_value, old_str, old_len);
    memcpy(new_str_value + old_len, add_str, add_len);
    new_str_value[new_len] = '\0';
    json_object_put(*jobj);
    *jobj = json_object_new_string(new_str_value);
    return json_object_get_string(*jobj);
}

static
const char* processor_setup_page(processor_state_t *self, json_object *request)
{
    json_object *page_obj = NULL;
    if (json_object_object_get_ex(request, "action", &page_obj)) {
        json_object_get(page_obj);
        json_object_object_del(request, "action");
    } else if (json_object_object_get_ex(request, "logjam_action", &page_obj)) {
        json_object_get(page_obj);
        json_object_object_del(request, "logjam_action");
    } else {
        page_obj = json_object_new_string("Unknown#unknown_method");
    }

    if (page_obj == NULL) {
        fprintf(stderr, "[E] missing action for request in stream: %s\n", self->stream_info->key);
        // dump_json_object(stderr, "[D] REQUEST", request);
        page_obj = json_object_new_string("Unknown#unknown_method");
    }

    const char *page_str = json_object_get_string(page_obj);

    if (strlen(page_str) == 0)
        page_str = append_to_json_string(&page_obj, page_str, "Unknown#unknown_method");
    else if (!strchr(page_str, '#'))
        page_str = append_to_json_string(&page_obj, page_str, "#unknown_method");
    else if (page_str[strlen(page_str)-1] == '#')
        page_str = append_to_json_string(&page_obj, page_str, "unknown_method");

    json_object_object_add(request, "page", page_obj);

    return page_str;
}

static
const char* processor_setup_module(processor_state_t *self, const char *page)
{
    int max_mod_len = strlen(page);
    char module_str[max_mod_len+1];
    char *mod_ptr = strchr(page, ':');
    strcpy(module_str, "::");
    if (mod_ptr != NULL){
        if (mod_ptr != page) {
            int mod_len = mod_ptr - page;
            memcpy(module_str+2, page, mod_len);
            module_str[mod_len+2] = '\0';
        }
    } else {
        char *action_ptr = strchr(page, '#');
        if (action_ptr != NULL) {
            int mod_len = action_ptr - page;
            memcpy(module_str+2, page, mod_len);
            module_str[mod_len+2] = '\0';
        }
    }
    char *module = zhash_lookup(self->modules, module_str);
    if (module == NULL) {
        module = strdup(module_str);
        int rc = zhash_insert(self->modules, module, module);
        assert(rc == 0);
        zhash_freefn(self->modules, module, free);
    }
    // printf("[D] page: %s\n", page);
    // printf("[D] module: %s\n", module);
    return module;
}

static
int processor_setup_response_code(processor_state_t *self, json_object *request)
{
    json_object *code_obj = NULL;
    int response_code = 500;
    if (json_object_object_get_ex(request, "code", &code_obj)) {
        response_code = json_object_get_int(code_obj);
        json_object_object_del(request, "code");
    }
    json_object_object_add(request, "response_code", json_object_new_int(response_code));
    // printf("[D] response_code: %d\n", response_code);
    return response_code;
}

static
double processor_setup_time(processor_state_t *self, json_object *request, const char *time_name, const char *duplicate)
{
    // TODO: might be better to drop requests without total_time
    double total_time;
    json_object *total_time_obj = NULL;
    if (json_object_object_get_ex(request, time_name, &total_time_obj)) {
        total_time = json_object_get_double(total_time_obj);
        if (total_time == 0.0) {
            total_time = 1.0;
            total_time_obj = json_object_new_double(total_time);
            json_object_object_add(request, time_name, total_time_obj);
        }
    } else {
        total_time = 1.0;
        total_time_obj = json_object_new_double(total_time);
        json_object_object_add(request, time_name, total_time_obj);
    }
    // printf("[D] %s: %f\n", time_name, total_time);
    if (duplicate) {
        // TODO: check whether we could simply share the object
        json_object_object_add(request, duplicate, json_object_new_double(total_time));
    }
    return total_time;
}

#if 0

static
const char* processor_setup_http_method(processor_state_t *self, json_object *request)
{
    json_object *request_info;
    if (!json_object_object_get_ex(request, "request_info", &request_info))
        return NULL;

    json_object *http_method_info;
    if (!json_object_object_get_ex(request_info, "method", &http_method_info))
        return NULL;

    if (json_object_get_type(http_method_info) != json_type_string)
        return NULL;

    const char *method = json_object_get_string(http_method_info);
    char *p = (char*)method;
    while (*p++)
        *p = toupper(*p);
    return method;
}

static
const char* processor_setup_host(processor_state_t *self, json_object *request)
{
    json_object *host_info;
    if (!json_object_object_get_ex(request, "host", &host_info))
        return "unknown";

    if (json_object_get_type(host_info) != json_type_string)
        return "unknown";

    return json_object_get_string(host_info);
}

static
const char* processor_setup_cluster(processor_state_t *self, json_object *request)
{
    json_object *cluster_info;
    if (!json_object_object_get_ex(request, "cluster", &cluster_info))
        return "unknown";

    if (json_object_get_type(cluster_info) != json_type_string)
        return "unknown";

    return json_object_get_string(cluster_info);
}

static
const char* processor_setup_datacenter(processor_state_t *self, json_object *request)
{
    json_object *datacenter_info;
    if (!json_object_object_get_ex(request, "datacenter", &datacenter_info))
        return "unknown";

    if (json_object_get_type(datacenter_info) != json_type_string)
        return "unknown";

    return json_object_get_string(datacenter_info);
}
#endif

static
int extract_severity_from_lines_object(json_object* lines)
{
    int log_level = -1;
    if (lines != NULL && json_object_get_type(lines) == json_type_array) {
        int array_len = json_object_array_length(lines);
        for (int i=0; i<array_len; i++) {
            json_object *line = json_object_array_get_idx(lines, i);
            if (line && json_object_get_type(line) == json_type_array) {
                json_object *level = json_object_array_get_idx(line, 0);
                if (level) {
                    int new_level = json_object_get_int(level);
                    if (new_level > log_level) {
                        log_level = new_level;
                    }
                }
            }
        }
    }
    // protect against unknown log levels
    return (log_level > 5) ? -1 : log_level;
}

static
int processor_setup_severity(processor_state_t *self, json_object *request)
{
    int severity = 1;
    json_object *severity_obj;
    if (json_object_object_get_ex(request, "severity", &severity_obj)) {
        severity = json_object_get_int(severity_obj);
    } else {
        json_object *lines_obj;
        if (json_object_object_get_ex(request, "lines", &lines_obj)) {
            int extracted_severity = extract_severity_from_lines_object(lines_obj);
            if (extracted_severity != -1) {
                severity = extracted_severity;
            }
        }
        severity_obj = json_object_new_int(severity);
        json_object_object_add(request, "severity", severity_obj);
    }
    return severity;
    // printf("[D] severity: %d\n\n", severity);
}

static
int processor_setup_minute(processor_state_t *self, json_object *request)
{
    // we know that started_at data is valid since we already checked that
    // when determining which processor to call
    int minute = 0;
    json_object *started_at_obj = NULL;
    if (json_object_object_get_ex(request, "started_at", &started_at_obj)) {
        const char *started_at = json_object_get_string(started_at_obj);
        char hours[3] = {started_at[11], started_at[12], '\0'};
        char minutes[3] = {started_at[14], started_at[15], '\0'};
        minute = 60 * atoi(hours) + atoi(minutes);
    }
    json_object *minute_obj = json_object_new_int(minute);
    json_object_object_add(request, "minute", minute_obj);
    // printf("[D] minute: %d\n", minute);
    return minute;
}

static
void processor_setup_other_time(processor_state_t *self, json_object *request, double total_time)
{
    double other_time = total_time;
    for (size_t i = 0; i <= last_other_time_resource_index; i++) {
        json_object *time_val;
        if (json_object_object_get_ex(request, other_time_resources[i], &time_val)) {
            double v = json_object_get_double(time_val);
            other_time -= v;
        }
    }
    json_object_object_add(request, "other_time", json_object_new_double(other_time));
    // printf("[D] other_time: %f\n", other_time);
}

static
void processor_setup_allocated_memory(processor_state_t *self, json_object *request)
{
    json_object *allocated_memory_obj;
    if (json_object_object_get_ex(request, "allocated_memory", &allocated_memory_obj))
        return;
    json_object *allocated_objects_obj;
    if (!json_object_object_get_ex(request, "allocated_objects", &allocated_objects_obj))
        return;
    json_object *allocated_bytes_obj;
    if (json_object_object_get_ex(request, "allocated_bytes", &allocated_bytes_obj)) {
        long allocated_objects = json_object_get_int64(allocated_objects_obj);
        long allocated_bytes = json_object_get_int64(allocated_bytes_obj);
        // assume 64bit ruby
        long allocated_memory = allocated_bytes + allocated_objects * 40;
        json_object_object_add(request, "allocated_memory", json_object_new_int64(allocated_memory));
        // printf("[D] allocated memory: %lu\n", allocated_memory);
    }
}

static
int processor_setup_heap_growth(processor_state_t *self, json_object *request)
{
    json_object *heap_growth_obj = NULL;
    int heap_growth = 0;
    if (json_object_object_get_ex(request, "heap_growth", &heap_growth_obj)) {
        heap_growth = json_object_get_int(heap_growth_obj);
    }
    // printf("[D] heap_growth: %d\n", heap_growth);
    return heap_growth;
}

static
json_object* processor_setup_exceptions(processor_state_t *self, json_object *request)
{
    json_object* exceptions = NULL;
    if (json_object_object_get_ex(request, "exceptions", &exceptions)) {
        int num_ex = json_object_array_length(exceptions);
        if (num_ex == 0) {
            json_object_object_del(request, "exceptions");
            return NULL;
        }
    }
    // dump_json_object(stdout, "[D] HARD EXCEPTIONS", exceptions);
    return exceptions;
}

static
json_object* processor_setup_soft_exceptions(processor_state_t *self, json_object *request)
{
  json_object* soft_exceptions = NULL;
  if (json_object_object_get_ex(request, "soft_exceptions", &soft_exceptions)) {
    int num_ex = json_object_array_length(soft_exceptions);
    if (num_ex == 0) {
      json_object_object_del(request, "soft_exceptions");
      return NULL;
    }
  }
  // dump_json_object(stdout, "[D] SOFT EXCEPTIONS", soft_exceptions);
  return soft_exceptions;
}

static
void processor_add_totals(processor_state_t *self, const char* namespace, increments_t *increments)
{
    increments_t *stored_increments = zhash_lookup(self->totals, namespace);
    if (stored_increments) {
        increments_add(stored_increments, increments);
    } else {
        increments_t *duped_increments = increments_clone(increments);
        int rc = zhash_insert(self->totals, namespace, duped_increments);
        assert(rc == 0);
        assert(zhash_freefn(self->totals, namespace, increments_destroy));
    }
}

static
const char *extract_agent_from_request(json_object *request)
{
    json_object *request_info;
    if (!json_object_object_get_ex(request, "request_info", &request_info))
        return NULL;

    json_object *headers;
    if (!json_object_object_get_ex(request_info, "headers", &headers))
        return NULL;

    json_object *user_agent;
    if (!json_object_object_get_ex(headers, "User-Agent", &user_agent))
        return NULL;

    return json_object_get_string(user_agent);
}

static
void processor_add_agent(processor_state_t *self, json_object *request)
{
    const char* agent = extract_agent_from_request(request);

    if (agent) {
        user_agent_stats_t *agent_stats = zhash_lookup(self->agents, agent);
        if (agent_stats == NULL) {
            agent_stats = zmalloc(sizeof(user_agent_stats_t));
            assert(agent_stats);
            int rc = zhash_insert(self->agents, agent, agent_stats);
            assert(rc == 0);
            zhash_freefn(self->agents, agent, free);
        }
        agent_stats->received_backend++;
    }
}

static
const char *extract_user_agent_from_request(json_object *request)
{
    json_object *user_agent;
    if (!json_object_object_get_ex(request, "user_agent", &user_agent))
        return NULL;

    return json_object_get_string(user_agent);
}

static
void processor_add_user_agent(processor_state_t *self, const char* agent, enum fe_msg_drop_reason reason)
{
    if (agent) {
        user_agent_stats_t *agent_stats = zhash_lookup(self->agents, agent);
        if (agent_stats == NULL) {
            agent_stats = zmalloc(sizeof(user_agent_stats_t));
            assert(agent_stats);
            int rc = zhash_insert(self->agents, agent, agent_stats);
            assert(rc == 0);
            zhash_freefn(self->agents, agent, free);
        }
        agent_stats->received_frontend++;
        agent_stats->fe_drop_reasons[reason]++;
        if (reason != FE_MSG_ACCEPTED)
            agent_stats->fe_dropped++;
    }
}

static
void processor_add_minutes(processor_state_t *self, const char* namespace, size_t minute, increments_t *increments)
{
    char key[2000];
    snprintf(key, 2000, "%lu-%s", minute, namespace);
    increments_t *stored_increments = zhash_lookup(self->minutes, key);
    if (stored_increments) {
        increments_add(stored_increments, increments);
    } else {
        increments_t *duped_increments = increments_clone(increments);
        int rc = zhash_insert(self->minutes, key, duped_increments);
        assert(rc == 0);
        assert(zhash_freefn(self->minutes, key, increments_destroy));
    }
}

#define QUANTS_ARRAY_SIZE (sizeof(size_t) * (last_resource_offset + 1))

static
void add_quant(const char* namespace, size_t resource_idx, char kind, size_t quant, zhash_t* quants)
{
    char key[2000];
    sprintf(key, "%c-%zu-%s", kind, quant, namespace);
    // printf("[D] QUANT-KEY: %s\n", key);
    size_t *stored = zhash_lookup(quants, key);
    if (stored == NULL) {
        stored = zmalloc(QUANTS_ARRAY_SIZE);
        zhash_insert(quants, key, stored);
        zhash_freefn(quants, key, free);
    }
    stored[resource_idx]++;
}

static double buckets[HISTOGRAM_SIZE+1] = {
    1,            //    1   ms               1 object            1   KB
    3,            //    3   ms               3 objects           3   KB
    10,           //   10   ms              10 objects          10   KB
    30,           //   30   ms              30 objects          30   KB
    100,          //  100   ms             100 objects         100   KB
    300,          //  300   ms             300 objects         300   KB
    1000,         //    1   second          1K objects       ~   1   MB
    3000,         //    3   seconds         2K objects       ~   2.9 MB
    10000,        //   10   seconds        10K objects       ~   9.7 MB
    30000,        //   30   seconds        30K objects       ~  29.3 MB
    100000,       //  100   seconds       100K objects       ~  97.6 MB
    300000,       //    5   minutes       300K objects       ~ 293   MB
    1000000,      // ~ 17   minutes         1M objects       ~ 976   MB
    3000000,      //   50   minutes         3M objects       ~   2.9 GB
    10000000,     //  ~ 2.6 hours          10M objects       ~   9.7 GB
    30000000,     //  ~ 8.3 hours          30M objects       ~  28.9 GB
    100000000,    //  ~ 1.2 days          100M objects       ~  96.3 GB
    300000000,    //    3.5 days          300M objects       ~ 289   GB
    1000000000,   //   11.6 days            1B objects       ~ 963   GB
    3000000000,   //   34.7 days            3B objects       ~   2.8 TB
    10000000000,  //  116   days           10B objects       ~   9.4 TB
    30000000000,  //  347   days           30B objects       ~  28.2 TB
    0
};


static inline size_t find_bucket(double value)
{
    double *p = buckets;
    while (*p < value && *(p+1) != 0) p++;
    assert(*p);
    return *p;
}

static inline size_t find_bucket_index(double value)
{
    double *p = buckets;
    size_t i = 0;
    while (*p < value && *(p+1) != 0) {
        i++;
        p++;
    }
    assert(*p);
    return i;
}

static
void processor_add_quants(processor_state_t *self, const char* namespace, increments_t *increments)
{
    for (size_t i=0; i<=last_resource_offset; i++){
        double val = increments->metrics[i].val;
        if (val > 0) {
            char kind;
            double d;
            double bucket;
            // printf("[D] trying to add quant: %zu=%s\n", i, i2r(i));
            if (i <= last_time_resource_offset) {
                kind = 't';
                bucket = find_bucket(val);
                d = 1;
            } else if (i == allocated_objects_index) {
                kind = 'm';
                d = 1;
            } else if (i == allocated_bytes_index) {
                kind = 'm';
                d = 1024;
            } else if ((i > last_heap_resource_offset) && (i <= last_frontend_resource_offset)) {
                kind = 'f';
                d = 1;
            } else {
                // printf("[D] skipping quant: %s\n", i2r(i));
                continue;
            }
            // This is stupid, but historic. We should actually store just the bucket
            // index and let the API in logjam convert bucket indexes to real values.
            if (d != 1)
                bucket = find_bucket(val/d) * d;
            else
                bucket = find_bucket(val);
            // printf("[D] determined bucket for %s, kind %c, for %f to be %f (factor %f)\n", i2r(i), kind, val, bucket, d);
            add_quant(namespace, i, kind, bucket, self->quants);
            add_quant("all_pages", i, kind, bucket, self->quants);
        }
    }
}

void dump_histogram(const char* key, size_t *h)
{
    char line[2000];
    int n = 0;
    for (int i = 0 ; i < HISTOGRAM_SIZE; i++) {
        n += sprintf(line+n, "%zu", h[i]);
        if (i < HISTOGRAM_SIZE - 1)
            n += sprintf(line+n, ", ");
    }
    printf("[D] HISTOGRAM: %s = [%s]\n", key, line);
}

void dump_histograms(zhash_t* histograms)
{
    size_t *h = zhash_first(histograms);
    while (h) {
        const char* key = zhash_cursor(histograms);
        dump_histogram(key, h);
        h = zhash_next(histograms);
    }
}


static
void processor_add_histogram(processor_state_t *self, const char* namespace, int minute, const char* resource, int time_index, increments_t *increments, json_object *request)
{
    char key[2000];
    snprintf(key, 2000, "%d-%s-%s", minute, resource, namespace);
    // printf("[D] HISTOGRAM-KEY: %s\n", key);

    double time = increments->metrics[time_index].val;
    if (time == 0) {
        fprintf(stderr, "[E] HISTOGRAM: expected %s to be greater zero\n", resource);
        dump_json_object(stderr, "[E] REQUEST", request);
        dump_increments(namespace, increments);
        return;
    }

    size_t *histogram = zhash_lookup(self->histograms, key);
    if (histogram == NULL) {
        histogram = zmalloc(HISTOGRAM_SIZE * sizeof(size_t));
        zhash_insert(self->histograms, key, histogram);
        zhash_freefn(self->histograms, key, free);
    }
    size_t i = find_bucket_index(time);
    assert(i < HISTOGRAM_SIZE);
    histogram[i]++;
    // dump_histogram(key, histogram);
    // dump_histograms(self->histograms);
}

static
bool slow_request(stream_info_t *stream_info, double total_time, const char* module)
{
    if (total_time > stream_info->import_threshold)
        return true;
    for (int i=0; i<stream_info->module_threshold_count; i++) {
        if (!strcmp(module, stream_info->module_thresholds[i].name)) {
            if (total_time > stream_info->module_thresholds[i].value)
                return true;
            else
                return false;
        }
    }
    return false;
}

static
bool sample_randomly(stream_info_t* info)
{
    double sampling_rate_threshold = info->sampling_rate_400s_threshold;
    if (sampling_rate_threshold == MAX_RANDOM_VALUE) {
        // printf("[D] processor: %s: taking 400 since sampling all requests\n", info ? info->key : "");
        return true;
    }
    if (random() <= sampling_rate_threshold) {
        // printf("[D] processor: %s: taking 400 since it matched threshold\n", info ? info->key : "");
        return true;
    }
    // printf("[D] processor: %s: rejecting 400 since it exceeded threshold\n", info ? info->key : "");
    return false;
}

static
sampling_reason_t interesting_request(request_data_t *request_data, json_object *request, stream_info_t* info)
{
    sampling_reason_t reason = 0;

    if (slow_request(info, request_data->total_time, request_data->module+2))
        reason |= SAMPLE_SLOW_REQUEST;

    if (request_data->severity >= LOG_SEVERITY_FATAL)
        reason |= SAMPLE_LOG_SEVERITY;
    else if (request_data->severity >= LOG_SEVERITY_ERROR && (request_data->response_code >= 500 || sample_randomly(info)))
        reason |= SAMPLE_LOG_SEVERITY;
    else if (request_data->severity >= LOG_SEVERITY_WARN && sample_randomly(info))
        reason |= SAMPLE_LOG_SEVERITY;

    if (request_data->response_code >= 500)
        reason |= SAMPLE_500;
    else if (request_data->response_code >= 400 && sample_randomly(info))
        reason |= SAMPLE_400;
    else if (request_data->response_code == 0)
        reason |= SAMPLE_000;

    if (request_data->exceptions != NULL)
        reason |= SAMPLE_EXCEPTIONS;

    if (request_data->heap_growth > 0)
        reason |= SAMPLE_HEAP_GROWTH;

    return reason;
}

static
void extract_request_path(request_data_t *request_data, json_object *request,  stream_info_t* info)
{
    request_data->path = NULL;
    json_object *req_info;
    if (json_object_object_get_ex(request, "request_info", &req_info)) {
        json_object *url_obj;
        if (!json_object_object_get_ex(req_info, "url", &url_obj)) {
            return;
        }
        const char *url = json_object_get_string(url_obj);
        if (url == NULL) {
            if (info)
                fprintf(stderr, "[W] got request with NULL url from %s\n", info->key);
            if (verbose)
                dump_json_object(stderr, "[W] REQUEST", request);
            return;
        }
        // skip over protocol and domain, if present.
        const char *p = strstr(url, "://");
        if (p)
            p += 3;
        else
            p = url;
        // find first slash
        while (*p && *p != '/')
            p++;
        request_data->path = p;
    }
}

static
int ignore_request(request_data_t *request_data, json_object *request, stream_info_t* info)
{
    json_object *logjam_ignore_message_obj;
    if (json_object_object_get_ex(request, "logjam_ignore_message", &logjam_ignore_message_obj)) {
        if (json_object_get_boolean(logjam_ignore_message_obj))
            // fprintf(stderr, "[D] ignored message because logjam_ignore_message was set to true");
            return 1;
    }
    if (request_data->path) {
        const char *prefix = info->ignored_request_prefix;
        if (prefix != NULL) {
            if (strstr(request_data->path, prefix) == request_data->path) {
                // fprintf(stderr, "[D] ignored request because ignored request prefix matched. url: %s\n", url);
                return 1;
            }
        }
    }
    return 0;
}

static
throttling_reason_t throttle_request(stream_info_t *stream)
{
    if (throttle_request_for_stream(stream))
        return THROTTLE_MAX_INSERTS_PER_SECOND;
    if (stream->storage_size > HARD_LIMIT_STORAGE_SIZE)
        return THROTTLE_HARD_LIMIT_STORAGE_SIZE;
    if (stream->storage_size > SOFT_LIMIT_STORAGE_SIZE && random() > TEN_PERCENT_OF_MAX_RANDOM)
        return THROTTLE_SOFT_LIMIT_STORAGE_SIZE;
    return NOT_THROTTLED;
}

static int
backend_only_request(const char *action, stream_info_t *stream)
{
    assert(action);

    if (stream->all_requests_are_backend_only_requests)
        return 1;

    if (stream->backend_only_requests_size == 0)
        return 0;

    int n = stream->backend_only_requests_size;
    for (int i = 0; i < n; i++)
        if (strstr(action, stream->backend_only_requests[i]) == action)
            return 1;

    return 0;
}

void processor_add_request(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    // dump_json_object(stdout, "[D] REQUEST", request);
    request_data_t request_data;
    extract_request_path(&request_data, request, self->stream_info);
    if (ignore_request(&request_data, request, self->stream_info)) return;

    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.response_code = processor_setup_response_code(self, request);
    request_data.severity = processor_setup_severity(self, request);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "total_time", NULL);

    request_data.exceptions = processor_setup_exceptions(self, request);
    request_data.soft_exceptions = processor_setup_soft_exceptions(self, request);
    processor_setup_other_time(self, request, request_data.total_time);
    processor_setup_allocated_memory(self, request);
    request_data.heap_growth = processor_setup_heap_growth(self, request);
    adjust_caller_info(request_data.path, request_data.module, request, self->stream_info);

    increments_t* increments = increments_new();
    increments->backend_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_apdex(increments, request_data.total_time);
    increments_fill_response_code(increments, &request_data);
    increments_fill_severity(increments, &request_data);
    increments_fill_caller_info(increments, request);
    increments_fill_sender_info(increments, request);
    increments_fill_exceptions(increments, request_data.exceptions);
    increments_fill_soft_exceptions(increments, request_data.soft_exceptions);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    processor_add_histogram(self, request_data.page, request_data.minute, "total_time", total_time_index, increments, request);
    processor_add_histogram(self, request_data.module, request_data.minute, "total_time", total_time_index, increments, request);
    processor_add_histogram(self, "all_pages", request_data.minute, "total_time", total_time_index, increments, request);

    increments_destroy(increments);

    processor_add_agent(self, request);

    if (!backend_only_request(request_data.page, self->stream_info)) {
        json_object *request_id_obj;
        if (json_object_object_get_ex(request, "request_id", &request_id_obj)) {
            const char *uuid = json_object_get_string(request_id_obj);
            if (uuid) {
                char app_env_uuid[1024] = {0};
                snprintf(app_env_uuid, 1024, "%s-%s", self->stream_info->key, uuid);
                tracker_add_uuid(pstate->tracker, app_env_uuid);
            }
        }
    } else {
        // printf("[D] ignored tracking for backend only request: %s\n", request_data.page);
    }

    if (0) {
        dump_json_object(stdout, "[D]", request);
        if (self->request_count % 100 == 0) {
            processor_dump_state(self);
        }
    }

    sampling_reason_t sampling_reason = interesting_request(&request_data, request, self->stream_info);
    if (!sampling_reason) {
        return;
    }
    // printf("[D] sampling: %s, reason: %x\n", request_data.page, sampling_reason);
    throttling_reason_t throttling_reason = throttle_request(self->stream_info);
    if (throttling_reason) {
        importer_prometheus_client_count_throttled_inserts_for_stream(self->stream_info, 1);
        // printf("[D] throttled: %s, reason: %s\n", request_data.page, throttling_reason_str(throttling_reason));
        return;
    }
    json_object_get(request);
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, self->db_name);
    zmsg_addstr(msg, "r");
    zmsg_addstr(msg, request_data.module);
    zmsg_addptr(msg, request);
    zmsg_addptr(msg, self->stream_info);
    reference_stream_info(self->stream_info);
    zmsg_addmem(msg, &sampling_reason, sizeof(sampling_reason_t));
    if (!output_socket_ready(pstate->push_socket, 0)) {
        fprintf(stderr, "[W] parser [%zu]: push socket not ready\n", pstate->id);
    }
    if (zmsg_send_with_retry(&msg, pstate->push_socket))
        release_stream_info(self->stream_info);
    else {
        __atomic_add_fetch(&queued_inserts, 1, __ATOMIC_SEQ_CST);
        importer_prometheus_client_count_inserts_for_stream(self->stream_info, 1);
    }
}

static
char* extract_page_for_jse(json_object *request)
{
    json_object *page_obj = NULL;
    if (json_object_object_get_ex(request, "logjam_action", &page_obj)) {
        page_obj = json_object_new_string(json_object_get_string(page_obj));
    } else {
        page_obj = json_object_new_string("Unknown#unknown_method");
    }

    const char *page_str = json_object_get_string(page_obj);

    if (strlen(page_str) == 0)
        page_str = append_to_json_string(&page_obj, page_str, "Unknown#unknown_method");
    else if (!strchr(page_str, '#'))
        page_str = append_to_json_string(&page_obj, page_str, "#unknown_method");
    else if (page_str[strlen(page_str)-1] == '#')
        page_str = append_to_json_string(&page_obj, page_str, "unknown_method");

    char *page = strdup(page_str);
    json_object_put(page_obj);
    return page;
}

static
char* exctract_key_from_jse_description(json_object *request)
{
    json_object *description_obj = NULL;
    const char *description;
    if (json_object_object_get_ex(request, "description", &description_obj)) {
        description = json_object_get_string(description_obj);
    } else {
        description = "unknown_exception";
    }
    char *result = strdup(description);
    return result;
}

void processor_add_js_exception(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    char *page = extract_page_for_jse(request);
    char *js_exception = exctract_key_from_jse_description(request);

    if (strlen(js_exception) == 0) {
        if (!quiet)
            fprintf(stderr, "[W] could not extract js_exception from request. ignoring.\n");
        dump_json_object(stderr, "[W]", request);
        free(page);
        free(js_exception);
        return;
    }

    int minute = processor_setup_minute(self, request);
    const char *module = processor_setup_module(self, page);

    increments_t* increments = increments_new();
    increments_fill_js_exception(increments, js_exception);

    processor_add_totals(self, "all_pages", increments);
    processor_add_minutes(self, "all_pages", minute, increments);

    if (strstr(page, "#unknown_method") == NULL) {
        processor_add_totals(self, page, increments);
        processor_add_minutes(self, page, minute, increments);
    }

    if (strcmp(module, "Unknown") != 0) {
        processor_add_totals(self, module, increments);
        processor_add_minutes(self, module, minute, increments);
    }

    increments_destroy(increments);
    free(page);
    free(js_exception);

    json_object_get(request);
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, self->db_name);
    zmsg_addstr(msg, "j");
    zmsg_addstr(msg, module);
    zmsg_addptr(msg, request);
    zmsg_addptr(msg, self->stream_info);
    reference_stream_info(self->stream_info);
    zmsg_send_with_retry(&msg, pstate->push_socket);
    __atomic_add_fetch(&queued_inserts, 1, __ATOMIC_SEQ_CST);
}

void processor_add_event(processor_state_t *self, parser_state_t *pstate, json_object *request)
{
    processor_setup_minute(self, request);
    json_object_get(request);
    zmsg_t *msg = zmsg_new();
    zmsg_addstr(msg, self->db_name);
    zmsg_addstr(msg, "e");
    zmsg_addstr(msg, "");
    zmsg_addptr(msg, request);
    zmsg_addptr(msg, self->stream_info);
    reference_stream_info(self->stream_info);
    zmsg_send_with_retry(&msg, pstate->push_socket);
    __atomic_add_fetch(&queued_inserts, 1, __ATOMIC_SEQ_CST);
}

static inline
bool sorted_ascending(int64_t *a, int n)
{
    for (int i=1; i<n; i++) {
        if (a[i] < a[i-1])
            return false;
    }
    return true;
}

static inline
bool all_zero(int64_t *a, int n)
{
    for (int i=0; i<n; i++) {
        if (a[i])
            return false;
    }
    return true;
}

#if 0
// correct negative values at the beginning of timestamps series
// often, request_start and response_start are equal and negative
// and everything before it is zero

static
void auto_correct_prefix(int64_t *a, int n)
{
    for (int i=0; i<n; i++) {
        if (a[i]<0)
            a[i] = 0;
        else if (a[i]>0)
            break;
    }
}
#endif

static
void make_relative(int64_t *a, int n, int64_t base)
{
    for (int i=0; i<n; i++) {
        if (a[i] > 0)
            a[i] -= base;
    }
}

static
int extract_frontend_timings(json_object *request, int64_t *timings, int num_timings, const char *type, const char **rts)
{
    json_object *rts_object = NULL;
    if (json_object_object_get_ex(request, "rts", &rts_object)) {
        *rts = json_object_get_string(rts_object);
        // fprintf(stdout, "[D] RTS: %s\n", rts);
    } else {
        *rts = NULL;
        if (verbose)
            fprintf(stderr, "[W] processor: dropped %s request without timing information\n", type);
        return 0;
    }
    int n = 0;
    const char *p = *rts;
    char c;
    int64_t *times = timings;
    int64_t value = 0;
    while (1) {
        c = *p++;
        if (c==',' || c==0) {
            times[n] = value;
            value = 0;
            n++;
            if (n == num_timings && c!=0) {
                if (verbose)
                    fprintf(stderr, "[W] processor: too many frontend timing values: %s\n", *rts);
                return 0;
            } else if (!c)
                break;
        } else {
            int x = c - '0';
            if (x < 0 || x > 9) {
                if (verbose)
                    fprintf(stderr, "[W] processor: invalid character in frontend timing information: %s\n", p-1);
                return 0;
            } else {
                value = value * 10 + x;
            }
        }
    }
    if (n < num_timings) {
        if (verbose)
            fprintf(stderr, "[W] processor: not enough timings: expected %d, got %d\n", num_timings, n);
        return 0;
    } else {
        // for (int i=0; i<num_timings; i++) {
        //     printf("[D] processor: time[%d]=%" PRIi64 "\n", i, timings[i]);
        // }
        return 1;
    }
}

#define NUM_TIMINGS 16
#define navigationStart 0
// #define unloadEventStart -1
// #define unloadEventEnd -1
// #define redirectStart -1
// #define redirectEnd -1
#define fetchStart 1
#define domainLookupStart 2
#define domainLookupEnd 3
#define connectStart 4
#define connectEnd 5
// #define secureConnectionStart -1
#define requestStart 6
#define responseStart 7
#define responseEnd 8
#define domLoading 9
#define domInteractive 10
#define domContentLoadedEventStart 11
#define domContentLoadedEventEnd 12
#define domComplete 13
#define loadEventStart 14
#define loadEventEnd 15

static int fe_apdex_attr_index = loadEventEnd;
int processor_set_frontend_apdex_attribute(const char *attr)
{
    if (streq(attr, "domInteractive")) {
        fe_apdex_attr_index = domInteractive;
        return 1;
    }
    if (streq(attr, "loadEventEnd")) {
        fe_apdex_attr_index = loadEventEnd;
        return 1;
    }
    return 0;
}

static
enum fe_msg_drop_reason convert_frontend_timings_to_json(json_object *request, int64_t *timings, int64_t *mtimes, const char* user_agent, const char* rts)
{
    int64_t base = timings[navigationStart];
    if (base == 0) {
        base = timings[fetchStart];
        timings[navigationStart] = base;
    }
    if (base == 0) {
        if (all_zero(timings, NUM_TIMINGS))
            return FE_MSG_NAV_TIMING;
        else
            return FE_MSG_INVALID;
    }
    make_relative(timings, NUM_TIMINGS, base);
    // auto_correct_prefix(timings, NUM_TIMINGS);

    int64_t utimes[] = {
        timings[navigationStart],
        timings[requestStart],
        timings[responseStart],
        timings[responseEnd],
        timings[domComplete],
        timings[loadEventEnd],
        timings[domInteractive]
    };

    if (frontend_timings) {
        fprintf(frontend_timings,
                "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",\"%s\",%s\n",
                utimes[0], utimes[1], utimes[2], utimes[3], utimes[4], utimes[5], utimes[6], user_agent, rts);
    }

    if (utimes[0] < 0 || utimes[6] <= 0 || !sorted_ascending(utimes, 5)) {
        // if (!frontend_timings) {
        //     fprintf(stderr,
        //             "[W] processor: dropped frontend request due to invalid timings: "
        //             "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",\"%s\"\n",
        //             utimes[0], utimes[1], utimes[2], utimes[3], utimes[4], utimes[5], utimes[6], user_agent);
        // }
        return FE_MSG_INVALID;
    }

    int64_t navigation_time = mtimes[0] = timings[fetchStart];
    int64_t connect_time    = mtimes[1] = timings[requestStart] - timings[fetchStart];
    int64_t request_time    = mtimes[2] = timings[responseStart] - timings[requestStart];
    int64_t response_time   = mtimes[3] = timings[responseEnd] - timings[responseStart];
    int64_t processing_time = mtimes[4] = timings[domComplete] - timings[responseEnd];
    int64_t load_time       = mtimes[5] = timings[loadEventEnd] - timings[domComplete];
    int64_t page_time       = mtimes[6] = timings[loadEventEnd];
    int64_t dom_interactive = mtimes[7] = timings[domInteractive];

    json_object_object_add(request, "navigation_time", json_object_new_int64(navigation_time));
    json_object_object_add(request, "connect_time", json_object_new_int64(connect_time));
    json_object_object_add(request, "request_time", json_object_new_int64(request_time));
    json_object_object_add(request, "response_time", json_object_new_int64(response_time));
    json_object_object_add(request, "processing_time", json_object_new_int64(processing_time));
    json_object_object_add(request, "load_time", json_object_new_int64(load_time));
    json_object_object_add(request, "page_time", json_object_new_int64(page_time));
    json_object_object_add(request, "dom_interactive", json_object_new_int64(dom_interactive));

    // dump_json_object(stdout, "[D]", request);

    return FE_MSG_ACCEPTED;
}

static
int check_frontend_request_validity(parser_state_t *pstate, json_object *request, const char* type, zmsg_t* msg)
{
    json_object *request_id_obj;
    const char *uuid = NULL;
    if (json_object_object_get_ex(request, "logjam_request_id", &request_id_obj)
        || json_object_object_get_ex(request, "request_id", &request_id_obj)) {
        uuid = json_object_get_string(request_id_obj);
    }
    if (!uuid) {
        if (verbose) {
            fprintf(stderr, "[W] processor: dropped %s request without request_id\n", type);
            dump_json_object(stderr, "[W]", request);
        }
        return 0;
    }
    if (tracker_delete_uuid(pstate->tracker, uuid, msg, type)) {
        // fprintf(stderr, "[D] processor: tracker found %s request with request_id: %s\n", uuid, type);
        // dump_json_object(stdout, "[D]", request);
        return 1;
    } else {
        // fprintf(stderr, "[D] processor: tracker could process %s request: request_id %s\n", type, uuid);
        // dump_json_object(stderr, "[D]", request);
        return 0;
    }
}

static const char* str_fe_reason(enum fe_msg_drop_reason reason)
{
    switch (reason){
    case FE_MSG_ACCEPTED:   return "FE_MSG_ACCEPTED";
    case FE_MSG_OUTLIER:    return "FE_MSG_OUTLIER";
    case FE_MSG_NAV_TIMING: return "FE_MSG_NAV_TIMING";
    case FE_MSG_ILLEGAL:    return "FE_MSG_ILLEGAL";
    case FE_MSG_CORRUPTED:  return "FE_MSG_CORRUPTED";
    case FE_MSG_INVALID:    return "FE_MSG_INVALID";
    default:                return "FE_MSG_UNKNOWN";
    }
}

static
void print_fe_drop_reason(const char* type, enum fe_msg_drop_reason reason)
{
    if (verbose)
        fprintf(stderr, "[W] processor: dropped %s request (%s)\n", type, str_fe_reason(reason));
}

enum fe_msg_drop_reason processor_add_frontend_data(processor_state_t *self, parser_state_t *pstate, json_object *request, zmsg_t* msg)
{
    // dump_json_object(stderr, "[D]", request);
    // if (self->request_count % 100 == 0) {
    //      processor_dump_state(self);
    // }

    const char* agent = extract_user_agent_from_request(request);
    enum fe_msg_drop_reason reason;

    int64_t timings[16];
    const char *rts;
    if (!extract_frontend_timings(request, timings, 16, "frontend", &rts)) {
        reason = FE_MSG_CORRUPTED;
        print_fe_drop_reason("frontend", reason);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    if (!check_frontend_request_validity(pstate, request, "frontend", msg)) {
        reason = FE_MSG_INVALID;
        print_fe_drop_reason("frontend", FE_MSG_INVALID);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    int64_t mtimes[8];
    reason = convert_frontend_timings_to_json(request, timings, mtimes, agent, rts);
    if (reason) {
        print_fe_drop_reason("frontend", reason);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    request_data_t request_data;
    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "page_time", "frontend_time");

    // TODO: revisit when switching to percentiles
    if (request_data.total_time > FE_MSG_OUTLIER_THRESHOLD_MS) {
        reason = FE_MSG_OUTLIER;
        print_fe_drop_reason("frontend", reason);
        // dump_json_object(stderr, "[W]", request);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    increments_t* increments = increments_new();
    increments->page_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_frontend_apdex(increments, request_data.total_time);
    increments_fill_page_apdex(increments, timings[fe_apdex_attr_index]);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    processor_add_histogram(self, request_data.page, request_data.minute, "page_time", page_time_index, increments, request);
    processor_add_histogram(self, request_data.module, request_data.minute, "page_time", page_time_index, increments, request);
    processor_add_histogram(self, "all_pages", request_data.minute, "page_time", page_time_index, increments, request);

    // dump_increments("add_frontend_data", increments);

    increments_destroy(increments);

    // TODO: store interesting requests
    reason = FE_MSG_ACCEPTED;
    processor_add_user_agent(self, agent, reason);
    return reason;
}

enum fe_msg_drop_reason processor_add_ajax_data(processor_state_t *self, parser_state_t *pstate, json_object *request, zmsg_t *msg)
{
    // dump_json_object(stdout, "[D]", request);
    // if (self->request_count % 100 == 0) {
    //     processor_dump_state(self);
    // }

    const char* agent = extract_user_agent_from_request(request);
    enum fe_msg_drop_reason reason;

    int64_t timings[2];
    const char *rts;
    if (!extract_frontend_timings(request, timings, 2, "ajax", &rts)) {
        reason = FE_MSG_CORRUPTED;
        print_fe_drop_reason("ajax", reason);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    if (!check_frontend_request_validity(pstate, request, "ajax", msg)) {
        reason = FE_MSG_ILLEGAL;
        print_fe_drop_reason("ajax", reason);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    int64_t ajax_time = timings[1] - timings[0];
    if (ajax_time < 0) {
        reason = FE_MSG_INVALID;
        print_fe_drop_reason("ajax", reason);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    json_object_object_add(request, "ajax_time", json_object_new_int64(ajax_time));

    request_data_t request_data;
    request_data.page = processor_setup_page(self, request);
    request_data.module = processor_setup_module(self, request_data.page);
    request_data.minute = processor_setup_minute(self, request);
    request_data.total_time = processor_setup_time(self, request, "ajax_time", "frontend_time");

    // TODO: revisit when switching to percentiles
    if (request_data.total_time > FE_MSG_OUTLIER_THRESHOLD_MS) {
        reason = FE_MSG_OUTLIER;
        print_fe_drop_reason("ajax", reason);
        // dump_json_object(stderr, request);
        processor_add_user_agent(self, agent, reason);
        return reason;
    }

    increments_t* increments = increments_new();
    increments->ajax_request_count = 1;
    increments_fill_metrics(increments, request);
    increments_fill_frontend_apdex(increments, request_data.total_time);
    increments_fill_ajax_apdex(increments, request_data.total_time);

    processor_add_totals(self, request_data.page, increments);
    processor_add_totals(self, request_data.module, increments);
    processor_add_totals(self, "all_pages", increments);

    processor_add_minutes(self, request_data.page, request_data.minute, increments);
    processor_add_minutes(self, request_data.module, request_data.minute, increments);
    processor_add_minutes(self, "all_pages", request_data.minute, increments);

    processor_add_quants(self, request_data.page, increments);

    processor_add_histogram(self, request_data.page, request_data.minute, "ajax_time", ajax_time_index, increments, request);
    processor_add_histogram(self, request_data.module, request_data.minute, "ajax_time", ajax_time_index, increments, request);
    processor_add_histogram(self, "all_pages", request_data.minute, "ajax_time", ajax_time_index, increments, request);

    // dump_increments("add_ajax_data", increments);

    increments_destroy(increments);

    // TODO: store interesting requests
    reason = FE_MSG_ACCEPTED;
    processor_add_user_agent(self, agent, reason);
    return reason;
}

