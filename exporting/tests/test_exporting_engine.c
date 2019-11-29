// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_exporting_engine.h"
#include "libnetdata/required_dummies.h"

RRDHOST *localhost;
netdata_rwlock_t rrd_rwlock;

char log_line[MAX_LOG_LINE + 1];

//static int test_exporting_config(void);

struct section {
    avl avl;                // the index entry of this section - this has to be first!

    uint32_t hash;          // a simple hash to speed up searching
    // we first compare hashes, and only if the hashes are equal we do string comparisons

    char *name;

    struct section *next;    // gloabl config_mutex protects just this

    struct config_option *values;
    avl_tree_lock values_index;

    netdata_mutex_t mutex;  // this locks only the writers, to ensure atomic updates
    // readers are protected using the rwlock in avl_tree_lock
};


int __test_appconfig_load(struct config *root, char *filename, int overwrite_used)
{
    static int init = 0;
    struct section *connector, *instance;

    if (!root && init) {
        //freez(connector.name);
        //freez(instance.name);
        add_connector_instance(NULL, (void *)0x01);
        init = 0;
        return 1;
    }

    connector = mallocz(sizeof(struct section));
    instance = mallocz(sizeof(struct section));

    connector->name = strdupz("graphite");
    instance->name = strdupz("test");
    add_connector_instance(connector, instance);
    init = 1;
    return 1;
}

static void test_exporting_config_01 (void **state)
{
    struct connector_instance local_ci;
    assert_ptr_equal(get_connector_instance(&local_ci), NULL);
    // Cleanup the internal structure
    add_connector_instance(NULL, (void *) 0x01);
}

static void test_exporting_config_02 (void **state)
{
    int exporting_config_exists;

    exporting_config_exists = __test_appconfig_load(&exporting_config, "/etc/netdata/exporting.conf", 0);
    assert_int_equal(exporting_config_exists, 1);
    __test_appconfig_load(NULL,NULL,0);     // cleanup
}


void init_connectors_in_tests(struct engine *engine)
{
    expect_function_call(__wrap_now_realtime_sec);
    will_return(__wrap_now_realtime_sec, 2);

    expect_function_call(__wrap_uv_thread_create);

    expect_value(__wrap_uv_thread_create, thread, &engine->connector_root->instance_root->thread);
    expect_value(__wrap_uv_thread_create, worker, simple_connector_worker);
    expect_value(__wrap_uv_thread_create, arg, engine->connector_root->instance_root);

    assert_int_equal(__real_init_connectors(engine), 0);

    assert_int_equal(engine->now, 2);
    assert_int_equal(engine->connector_root->instance_root->after, 2);
}

static void test_exporting_engine(void **state)
{
    struct engine *engine = *state;

    expect_function_call(__wrap_read_exporting_config);
    will_return(__wrap_read_exporting_config, engine);

    expect_function_call(__wrap_init_connectors);
    expect_memory(__wrap_init_connectors, engine, engine, sizeof(struct engine));
    will_return(__wrap_init_connectors, 0);

    expect_function_call(__wrap_now_realtime_sec);
    will_return(__wrap_now_realtime_sec, 2);

    expect_function_call(__wrap_mark_scheduled_instances);
    expect_memory(__wrap_mark_scheduled_instances, engine, engine, sizeof(struct engine));
    will_return(__wrap_mark_scheduled_instances, 1);

    expect_function_call(__wrap_prepare_buffers);
    expect_memory(__wrap_prepare_buffers, engine, engine, sizeof(struct engine));
    will_return(__wrap_prepare_buffers, 0);

    expect_function_call(__wrap_notify_workers);
    expect_memory(__wrap_notify_workers, engine, engine, sizeof(struct engine));
    will_return(__wrap_notify_workers, 0);

    expect_function_call(__wrap_send_internal_metrics);
    expect_memory(__wrap_send_internal_metrics, engine, engine, sizeof(struct engine));
    will_return(__wrap_send_internal_metrics, 0);

    void *ptr = malloc(sizeof(int));
    assert_ptr_equal(exporting_main(ptr), NULL);
    assert_int_equal(engine->now, 2);
    free(ptr);
}

static void test_read_exporting_config(void **state)
{
    struct engine *engine = __mock_read_exporting_config(); // TODO: use real read_exporting_config() function
    *state = engine;

    assert_ptr_not_equal(engine, NULL);
    assert_string_equal(engine->config.prefix, "netdata");
    assert_string_equal(engine->config.hostname, "test-host");
    assert_int_equal(engine->config.update_every, 3);
    assert_int_equal(engine->instance_num, 0);

    struct connector *connector = engine->connector_root;
    assert_ptr_not_equal(connector, NULL);
    assert_ptr_equal(connector->next, NULL);
    assert_ptr_equal(connector->engine, engine);
    assert_int_equal(connector->config.type, BACKEND_TYPE_GRAPHITE);

    struct instance *instance = connector->instance_root;
    assert_ptr_not_equal(instance, NULL);
    assert_ptr_equal(instance->next, NULL);
    assert_ptr_equal(instance->connector, connector);
    assert_string_equal(instance->config.destination, "localhost");
    assert_int_equal(instance->config.update_every, 1);
    assert_int_equal(instance->config.buffer_on_failures, 10);
    assert_int_equal(instance->config.timeoutms, 10000);
    assert_true(simple_pattern_matches(instance->config.charts_pattern, "any_chart"));
    assert_true(simple_pattern_matches(instance->config.hosts_pattern, "anyt_host"));
    assert_int_equal(instance->config.options, EXPORTING_SOURCE_DATA_AS_COLLECTED | EXPORTING_OPTION_SEND_NAMES);

    teardown_configured_engine(state);
}

static void test_init_connectors(void **state)
{
    struct engine *engine = *state;

    init_connectors_in_tests(engine);

    assert_int_equal(engine->instance_num, 1);

    struct connector *connector = engine->connector_root;
    assert_ptr_equal(connector->next, NULL);
    assert_ptr_equal(connector->worker, simple_connector_worker);

    struct simple_connector_config *connector_specific_config = connector->config.connector_specific_config;
    assert_int_equal(connector_specific_config->default_port, 2003);

    struct instance *instance = connector->instance_root;
    assert_ptr_equal(instance->next, NULL);
    assert_int_equal(instance->index, 0);
    assert_ptr_equal(instance->start_batch_formatting, NULL);
    assert_ptr_equal(instance->start_host_formatting, NULL);
    assert_ptr_equal(instance->start_chart_formatting, NULL);
    assert_ptr_equal(instance->metric_formatting, format_dimension_collected_graphite_plaintext);
    assert_ptr_equal(instance->end_chart_formatting, NULL);
    assert_ptr_equal(instance->end_host_formatting, NULL);
    assert_ptr_equal(instance->end_batch_formatting, NULL);

    BUFFER *buffer = instance->buffer;
    assert_ptr_not_equal(buffer, NULL);
    buffer_sprintf(buffer, "%s", "graphite test");
    assert_string_equal(buffer_tostring(buffer), "graphite test");
}

static void test_init_graphite_instance(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;

    instance->config.options = EXPORTING_SOURCE_DATA_AVERAGE | EXPORTING_OPTION_SEND_NAMES;

    assert_int_equal(init_graphite_instance(instance), 0);
    assert_ptr_equal(instance->metric_formatting, format_dimension_stored_graphite_plaintext);
}

static void test_mark_scheduled_instances(void **state)
{
    struct engine *engine = *state;

    assert_int_equal(__real_mark_scheduled_instances(engine), 1);

    struct instance *instance = engine->connector_root->instance_root;
    assert_int_equal(instance->scheduled, 1);
    assert_int_equal(instance->before, 2);
}

static void test_rrdhost_is_exportable(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;

    expect_function_call(__wrap_info_int);

    assert_ptr_equal(localhost->exporting_flags, NULL);

    assert_int_equal(__real_rrdhost_is_exportable(instance, localhost), 1);

    assert_string_equal(log_line, "enabled exporting of host 'localhost' for instance 'instance_name'");

    assert_ptr_not_equal(localhost->exporting_flags, NULL);
    assert_int_equal(localhost->exporting_flags[0], RRDHOST_FLAG_BACKEND_SEND);
}

static void test_false_rrdhost_is_exportable(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;

    simple_pattern_free(instance->config.hosts_pattern);
    instance->config.hosts_pattern = simple_pattern_create("!*", NULL, SIMPLE_PATTERN_EXACT);

    expect_function_call(__wrap_info_int);

    assert_ptr_equal(localhost->exporting_flags, NULL);

    assert_int_equal(__real_rrdhost_is_exportable(instance, localhost), 0);

    assert_string_equal(log_line, "disabled exporting of host 'localhost' for instance 'instance_name'");

    assert_ptr_not_equal(localhost->exporting_flags, NULL);
    assert_int_equal(localhost->exporting_flags[0], RRDHOST_FLAG_BACKEND_DONT_SEND);
}

static void test_rrdset_is_exportable(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    RRDSET *st = localhost->rrdset_root;

    assert_ptr_equal(st->exporting_flags, NULL);

    assert_int_equal(__real_rrdset_is_exportable(instance, st), 1);

    assert_ptr_not_equal(st->exporting_flags, NULL);
    assert_int_equal(st->exporting_flags[0], RRDSET_FLAG_BACKEND_SEND);
}

static void test_false_rrdset_is_exportable(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    RRDSET *st = localhost->rrdset_root;

    simple_pattern_free(instance->config.charts_pattern);
    instance->config.charts_pattern = simple_pattern_create("!*", NULL, SIMPLE_PATTERN_EXACT);

    assert_ptr_equal(st->exporting_flags, NULL);

    assert_int_equal(__real_rrdset_is_exportable(instance, st), 0);

    assert_ptr_not_equal(st->exporting_flags, NULL);
    assert_int_equal(st->exporting_flags[0], RRDSET_FLAG_BACKEND_IGNORE);
}

static void test_exporting_calculate_value_from_stored_data(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    RRDDIM *rd = localhost->rrdset_root->dimensions;
    time_t timestamp;

    instance->after = 3;
    instance->before = 10;

    expect_function_call(__mock_rrddim_query_oldest_time);
    will_return(__mock_rrddim_query_oldest_time, 1);

    expect_function_call(__mock_rrddim_query_latest_time);
    will_return(__mock_rrddim_query_latest_time, 2);

    expect_function_call(__mock_rrddim_query_init);
    expect_value(__mock_rrddim_query_init, start_time, 1);
    expect_value(__mock_rrddim_query_init, end_time, 2);

    expect_function_call(__mock_rrddim_query_is_finished);
    will_return(__mock_rrddim_query_is_finished, 0);
    expect_function_call(__mock_rrddim_query_next_metric);
    will_return(__mock_rrddim_query_next_metric, pack_storage_number(27, SN_EXISTS));

    expect_function_call(__mock_rrddim_query_is_finished);
    will_return(__mock_rrddim_query_is_finished, 0);
    expect_function_call(__mock_rrddim_query_next_metric);
    will_return(__mock_rrddim_query_next_metric, pack_storage_number(45, SN_EXISTS));

    expect_function_call(__mock_rrddim_query_is_finished);
    will_return(__mock_rrddim_query_is_finished, 1);

    expect_function_call(__mock_rrddim_query_finalize);

    assert_int_equal(__real_exporting_calculate_value_from_stored_data(instance, rd, &timestamp), 36);
}

static void test_prepare_buffers(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;

    instance->start_batch_formatting = __mock_start_batch_formatting;
    instance->start_host_formatting = __mock_start_host_formatting;
    instance->start_chart_formatting = __mock_start_chart_formatting;
    instance->metric_formatting = __mock_metric_formatting;
    instance->end_chart_formatting = __mock_end_chart_formatting;
    instance->end_host_formatting = __mock_end_host_formatting;
    instance->end_batch_formatting = __mock_end_batch_formatting;
    __real_mark_scheduled_instances(engine);

    expect_function_call(__mock_start_batch_formatting);
    expect_value(__mock_start_batch_formatting, instance, instance);
    will_return(__mock_start_batch_formatting, 0);

    expect_function_call(__wrap_rrdhost_is_exportable);
    expect_value(__wrap_rrdhost_is_exportable, instance, instance);
    expect_value(__wrap_rrdhost_is_exportable, host, localhost);
    will_return(__wrap_rrdhost_is_exportable, 1);

    expect_function_call(__mock_start_host_formatting);
    expect_value(__mock_start_host_formatting, instance, instance);
    expect_value(__mock_start_host_formatting, host, localhost);
    will_return(__mock_start_host_formatting, 0);

    RRDSET *st = localhost->rrdset_root;
    expect_function_call(__wrap_rrdset_is_exportable);
    expect_value(__wrap_rrdset_is_exportable, instance, instance);
    expect_value(__wrap_rrdset_is_exportable, st, st);
    will_return(__wrap_rrdset_is_exportable, 1);

    expect_function_call(__mock_start_chart_formatting);
    expect_value(__mock_start_chart_formatting, instance, instance);
    expect_value(__mock_start_chart_formatting, st, st);
    will_return(__mock_start_chart_formatting, 0);

    RRDDIM *rd = localhost->rrdset_root->dimensions;
    expect_function_call(__mock_metric_formatting);
    expect_value(__mock_metric_formatting, instance, instance);
    expect_value(__mock_metric_formatting, rd, rd);
    will_return(__mock_metric_formatting, 0);

    expect_function_call(__mock_end_chart_formatting);
    expect_value(__mock_end_chart_formatting, instance, instance);
    expect_value(__mock_end_chart_formatting, st, st);
    will_return(__mock_end_chart_formatting, 0);

    expect_function_call(__mock_end_host_formatting);
    expect_value(__mock_end_host_formatting, instance, instance);
    expect_value(__mock_end_host_formatting, host, localhost);
    will_return(__mock_end_host_formatting, 0);

    expect_function_call(__mock_end_batch_formatting);
    expect_value(__mock_end_batch_formatting, instance, instance);
    will_return(__mock_end_batch_formatting, 0);

    assert_int_equal(__real_prepare_buffers(engine), 0);

    assert_int_equal(instance->stats.chart_buffered_metrics, 1);

    // check with NULL functions
    instance->start_batch_formatting = NULL;
    instance->start_host_formatting = NULL;
    instance->start_chart_formatting = NULL;
    instance->metric_formatting = NULL;
    instance->end_chart_formatting = NULL;
    instance->end_host_formatting = NULL;
    instance->end_batch_formatting = NULL;
    assert_int_equal(__real_prepare_buffers(engine), 0);

    assert_int_equal(instance->scheduled, 0);
    assert_int_equal(instance->after, 2);
}

static void test_exporting_name_copy(void **state)
{
    (void)state;

    char *source_name = "test.name-with/special#characters_";
    char destination_name[RRD_ID_LENGTH_MAX + 1];

    assert_int_equal(exporting_name_copy(destination_name, source_name, RRD_ID_LENGTH_MAX), 34);
    assert_string_equal(destination_name, "test.name_with_special_characters_");
}

static void test_format_dimension_collected_graphite_plaintext(void **state)
{
    struct engine *engine = *state;

    RRDDIM *rd = localhost->rrdset_root->dimensions;
    assert_int_equal(format_dimension_collected_graphite_plaintext(engine->connector_root->instance_root, rd), 0);
    assert_string_equal(
        buffer_tostring(engine->connector_root->instance_root->buffer),
        "netdata.test-host.chart_name.dimension_name;TAG1=VALUE1 TAG2=VALUE2 123000321 15051\n");
}

static void test_format_dimension_stored_graphite_plaintext(void **state)
{
    struct engine *engine = *state;

    expect_function_call(__wrap_exporting_calculate_value_from_stored_data);
    will_return(__wrap_exporting_calculate_value_from_stored_data, pack_storage_number(27, SN_EXISTS));

    RRDDIM *rd = localhost->rrdset_root->dimensions;
    assert_int_equal(format_dimension_stored_graphite_plaintext(engine->connector_root->instance_root, rd), 0);
    assert_string_equal(
        buffer_tostring(engine->connector_root->instance_root->buffer),
        "netdata.test-host.chart_name.dimension_name;TAG1=VALUE1 TAG2=VALUE2 690565856.0000000 15052\n");
}

static void test_exporting_discard_response(void **state)
{
    struct engine *engine = *state;

    BUFFER *response = buffer_create(0);
    buffer_sprintf(response, "Test response");

    expect_function_call(__wrap_info_int);

    assert_int_equal(exporting_discard_response(response, engine->connector_root->instance_root), 0);

    assert_string_equal(
        log_line,
        "EXPORTING: received 13 bytes from instance_name connector instance. Ignoring them. Sample: 'Test response'");

    assert_int_equal(buffer_strlen(response), 0);

    buffer_free(response);
}

static void test_simple_connector_receive_response(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    struct stats *stats = &instance->stats;

    int sock = 1;

    expect_function_call(__wrap_recv);
    expect_value(__wrap_recv, sockfd, 1);
    expect_not_value(__wrap_recv, buf, 0);
    expect_value(__wrap_recv, len, 4096);
    expect_value(__wrap_recv, flags, MSG_DONTWAIT);

    expect_function_call(__wrap_info_int);

    simple_connector_receive_response(&sock, instance);

    assert_string_equal(
        log_line,
        "EXPORTING: received 9 bytes from instance_name connector instance. Ignoring them. Sample: 'Test recv'");

    assert_int_equal(stats->chart_received_bytes, 9);
    assert_int_equal(stats->chart_receptions, 1);
    assert_int_equal(sock, 1);
}

static void test_simple_connector_send_buffer(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    struct stats *stats = &instance->stats;
    BUFFER *buffer = instance->buffer;

    int sock = 1;
    int failures = 3;

    __real_mark_scheduled_instances(engine);

    expect_function_call(__wrap_rrdhost_is_exportable);
    expect_value(__wrap_rrdhost_is_exportable, instance, instance);
    expect_value(__wrap_rrdhost_is_exportable, host, localhost);
    will_return(__wrap_rrdhost_is_exportable, 1);

    RRDSET *st = localhost->rrdset_root;
    expect_function_call(__wrap_rrdset_is_exportable);
    expect_value(__wrap_rrdset_is_exportable, instance, instance);
    expect_value(__wrap_rrdset_is_exportable, st, st);
    will_return(__wrap_rrdset_is_exportable, 1);

    __real_prepare_buffers(engine);

    expect_function_call(__wrap_send);
    expect_value(__wrap_send, sockfd, 1);
    expect_value(__wrap_send, buf, buffer_tostring(buffer));
    expect_string(
        __wrap_send, buf, "netdata.test-host.chart_name.dimension_name;TAG1=VALUE1 TAG2=VALUE2 123000321 15051\n");
    expect_value(__wrap_send, len, 84);
    expect_value(__wrap_send, flags, MSG_NOSIGNAL);

    simple_connector_send_buffer(&sock, &failures, instance);

    assert_int_equal(failures, 0);
    assert_int_equal(stats->chart_transmission_successes, 1);
    assert_int_equal(stats->chart_sent_bytes, 84);
    assert_int_equal(stats->chart_sent_metrics, 1);
    assert_int_equal(stats->chart_transmission_failures, 0);

    assert_int_equal(buffer_strlen(buffer), 0);

    assert_int_equal(sock, 1);
}

static void test_simple_connector_worker(void **state)
{
    struct engine *engine = *state;
    struct instance *instance = engine->connector_root->instance_root;
    BUFFER *buffer = instance->buffer;

    __real_mark_scheduled_instances(engine);

    expect_function_call(__wrap_rrdhost_is_exportable);
    expect_value(__wrap_rrdhost_is_exportable, instance, instance);
    expect_value(__wrap_rrdhost_is_exportable, host, localhost);
    will_return(__wrap_rrdhost_is_exportable, 1);

    RRDSET *st = localhost->rrdset_root;
    expect_function_call(__wrap_rrdset_is_exportable);
    expect_value(__wrap_rrdset_is_exportable, instance, instance);
    expect_value(__wrap_rrdset_is_exportable, st, st);
    will_return(__wrap_rrdset_is_exportable, 1);

    __real_prepare_buffers(engine);

    expect_function_call(__wrap_connect_to_one_of);
    expect_string(__wrap_connect_to_one_of, destination, "localhost");
    expect_value(__wrap_connect_to_one_of, default_port, 2003);
    expect_not_value(__wrap_connect_to_one_of, reconnects_counter, 0);
    expect_value(__wrap_connect_to_one_of, connected_to, 0);
    expect_value(__wrap_connect_to_one_of, connected_to_size, 0);
    will_return(__wrap_connect_to_one_of, 2);

    expect_function_call(__wrap_send);
    expect_value(__wrap_send, sockfd, 2);
    expect_value(__wrap_send, buf, buffer_tostring(buffer));
    expect_string(
        __wrap_send, buf, "netdata.test-host.chart_name.dimension_name;TAG1=VALUE1 TAG2=VALUE2 123000321 15051\n");
    expect_value(__wrap_send, len, 84);
    expect_value(__wrap_send, flags, MSG_NOSIGNAL);

    simple_connector_worker(instance);
}

static int test_exporting_config(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_exporting_config_01)//,
//
    };

    return cmocka_run_group_tests_name("exporting_config", tests, NULL, NULL);
};

int main(void)
{
    // Run tests for the config file itself
    test_exporting_config();

    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_exporting_engine, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test(test_read_exporting_config),
        cmocka_unit_test_setup_teardown(test_init_connectors, setup_configured_engine, teardown_configured_engine),
        cmocka_unit_test_setup_teardown(
            test_init_graphite_instance, setup_configured_engine, teardown_configured_engine),
        cmocka_unit_test_setup_teardown(
            test_mark_scheduled_instances, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_rrdhost_is_exportable, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_false_rrdhost_is_exportable, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_rrdset_is_exportable, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_false_rrdset_is_exportable, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_exporting_calculate_value_from_stored_data, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(test_prepare_buffers, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test(test_exporting_name_copy),
        cmocka_unit_test_setup_teardown(
            test_format_dimension_collected_graphite_plaintext, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_format_dimension_stored_graphite_plaintext, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_exporting_discard_response, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_simple_connector_receive_response, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_simple_connector_send_buffer, setup_initialized_engine, teardown_initialized_engine),
        cmocka_unit_test_setup_teardown(
            test_simple_connector_worker, setup_initialized_engine, teardown_initialized_engine),
    };

    return cmocka_run_group_tests_name("exporting_engine", tests, NULL, NULL);
}
