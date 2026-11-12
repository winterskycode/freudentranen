/* TOTK_VK_SHADERSTATS=1 (diagnostic): the driver's compile statistics of each game shader, from
   VK_KHR_pipeline_executable_properties, logged once per shader and stage as
   [shaderstats] <shader key> <stage> <statistic>=<value> ...
   (Turnip: max waves per core, instruction and register counts, nops, moves, conversions, sync stalls, private memory
   loads and stores, constants). Each pipeline's executables are matched to our shaders through the module of their stage. */
static int shaderstats_ok;
static PFN_vkGetPipelineExecutablePropertiesKHR shaderstats_props;
static PFN_vkGetPipelineExecutableStatisticsKHR shaderstats_stats;
static int shaderstats_wanted(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_SHADERSTATS"); on = e && atoi(e) > 0; } return on; }
/* the game's shader modules: module -> shader key, and which stages were logged for it */
#define SHADERSTATS_SLOTS 65536
static struct { VkShaderModule module; const char *key; uint32_t logged; } shaderstats_map[SHADERSTATS_SLOTS];
static pthread_mutex_t shaderstats_lock = PTHREAD_MUTEX_INITIALIZER;
static void shaderstats_note_module(VkShaderModule module, const char *key) {
    if (shaderstats_ok <= 0 || !module) return;
    pthread_mutex_lock(&shaderstats_lock);
    unsigned h = (unsigned)(((uint64_t)(uintptr_t)module * 0x9e3779b97f4a7c15ull) >> 48);
    for (unsigned n = 0; n < SHADERSTATS_SLOTS; n++, h = (h + 1) & (SHADERSTATS_SLOTS - 1))
        if (!shaderstats_map[h].module || shaderstats_map[h].module == module) { shaderstats_map[h].module = module; shaderstats_map[h].key = key; shaderstats_map[h].logged = 0; break; }
    pthread_mutex_unlock(&shaderstats_lock);
}
static void shaderstats_pipeline(VkPipeline pipeline, const VkPipelineShaderStageCreateInfo *stages, uint32_t count) {
    if (shaderstats_ok <= 0 || !pipeline || !count) return;
    VkPipelineInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
    uint32_t n = 0; if (shaderstats_props(device, &pi, &n, NULL) != VK_SUCCESS || !n) return;
    VkPipelineExecutablePropertiesKHR props[8]; if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) props[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    if (shaderstats_props(device, &pi, &n, props) < 0) return;
    for (uint32_t e = 0; e < n; e++) {
        const VkPipelineShaderStageCreateInfo *st = NULL;
        for (uint32_t s = 0; s < count; s++) if (stages[s].stage & props[e].stages) st = &stages[s];
        if (!st || !st->module) continue;
        /* one line per shader and executable kind (a vertex shader's binning variant is its own executable) */
        uint32_t bit = 1u << (e < 31 ? e : 31);
        pthread_mutex_lock(&shaderstats_lock);
        unsigned h = (unsigned)(((uint64_t)(uintptr_t)st->module * 0x9e3779b97f4a7c15ull) >> 48); const char *key = NULL; int fresh = 0;
        for (unsigned k = 0; k < SHADERSTATS_SLOTS && shaderstats_map[h].module; k++, h = (h + 1) & (SHADERSTATS_SLOTS - 1))
            if (shaderstats_map[h].module == st->module) { key = shaderstats_map[h].key; fresh = !(shaderstats_map[h].logged & bit); shaderstats_map[h].logged |= bit; break; }
        pthread_mutex_unlock(&shaderstats_lock);
        if (!key || !fresh) continue;
        VkPipelineExecutableInfoKHR xi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipeline, .executableIndex = e};
        uint32_t ns = 0; if (shaderstats_stats(device, &xi, &ns, NULL) != VK_SUCCESS || !ns) continue;
        VkPipelineExecutableStatisticKHR stats[48]; if (ns > 48) ns = 48;
        for (uint32_t i = 0; i < ns; i++) stats[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        if (shaderstats_stats(device, &xi, &ns, stats) < 0) continue;
        char line[2048]; int k = snprintf(line, sizeof line, "[shaderstats] %s %s", key, props[e].name);
        for (uint32_t i = 0; i < ns && k < (int)sizeof line - 64; i++) {
            char name[48]; int m = 0; for (const char *c = stats[i].name; *c && m < (int)sizeof name - 1; c++) if (*c != ' ') name[m++] = *c; name[m] = 0;
            switch (stats[i].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: k += snprintf(line + k, sizeof line - k, " %s=%u", name, stats[i].value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: k += snprintf(line + k, sizeof line - k, " %s=%lld", name, (long long)stats[i].value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: k += snprintf(line + k, sizeof line - k, " %s=%llu", name, (unsigned long long)stats[i].value.u64); break;
            default: k += snprintf(line + k, sizeof line - k, " %s=%g", name, stats[i].value.f64); break;
            }
        }
        LOG("%s", line);
    }
}
