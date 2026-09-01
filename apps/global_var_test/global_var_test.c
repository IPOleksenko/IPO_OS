#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <syscall.h>

typedef struct {
    int id;
    float value;
    char label[16];
} custom_config_t;

int main(int argc, char **argv) {
    const char *name = (argc > 1 && argv[1][0] != '\0') ? argv[1] : "app_counter_int";

    const char *int_name = "app_counter_int";
    const char *float_name = "app_float_value";
    const char *str_name = "app_status_text";
    const char *bool_name = "app_enabled_flag";
    const char *custom_name = "app_runtime_cfg";

    int int_value = 42;
    float float_value = 3.125f;
    char str_value[] = "hello from app";
    bool bool_value = true;
    custom_config_t custom = {
        .id = 77,
        .value = 9.5f,
        .label = "cfg_v1"
    };

    printf("[global-var-test] active name=%s\n", name);
    serial_printf("[global-var-test] active name=%s\n", name);

    int int_out = 0;
    int int_rc = IPO_VAR_SET(int_name, int_value);
    printf("[global-var-test] set %s => int=%d (rc=%d)\n", int_name, int_value, int_rc);
    serial_printf("[global-var-test] set %s => int=%d (rc=%d)\n", int_name, int_value, int_rc);
    int int_get_rc = IPO_VAR_GET(int_name, &int_out);
    printf("[global-var-test] get %s => int=%d (rc=%d)\n", int_name, int_out, int_get_rc);
    serial_printf("[global-var-test] get %s => int=%d (rc=%d)\n", int_name, int_out, int_get_rc);

    float float_out = 0.0f;
    int float_rc = IPO_VAR_SET(float_name, float_value);
    printf("[global-var-test] set %s => float=%f (rc=%d)\n", float_name, float_value, float_rc);
    serial_printf("[global-var-test] set %s => float=%f (rc=%d)\n", float_name, float_value, float_rc);
    int float_get_rc = IPO_VAR_GET(float_name, &float_out);
    printf("[global-var-test] get %s => float=%f (rc=%d)\n", float_name, float_out, float_get_rc);
    serial_printf("[global-var-test] get %s => float=%f (rc=%d)\n", float_name, float_out, float_get_rc);

    char str_out[64];
    memset(str_out, 0, sizeof(str_out));
    int str_rc = IPO_VAR_SET_STR(str_name, str_value);
    printf("[global-var-test] set %s => '%s' (rc=%d)\n", str_name, str_value, str_rc);
    serial_printf("[global-var-test] set %s => '%s' (rc=%d)\n", str_name, str_value, str_rc);
    int str_get_rc = IPO_VAR_GET_STR(str_name, str_out, sizeof(str_out));
    printf("[global-var-test] get %s => '%s' (rc=%d)\n", str_name, str_out, str_get_rc);
    serial_printf("[global-var-test] get %s => '%s' (rc=%d)\n", str_name, str_out, str_get_rc);

    int bool_flag = 1;
    int bool_rc = IPO_VAR_SET(bool_name, bool_flag);
    printf("[global-var-test] set %s => %d (rc=%d)\n", bool_name, bool_flag, bool_rc);
    serial_printf("[global-var-test] set %s => %d (rc=%d)\n", bool_name, bool_flag, bool_rc);
    int bool_out = 0;
    int bool_get_rc = IPO_VAR_GET(bool_name, &bool_out);
    printf("[global-var-test] get %s => %d (rc=%d)\n", bool_name, bool_out, bool_get_rc);
    serial_printf("[global-var-test] get %s => %d (rc=%d)\n", bool_name, bool_out, bool_get_rc);

    custom_config_t custom_out = {0};
    int custom_rc = IPO_VAR_SET(custom_name, custom);
    printf("[global-var-test] set %s => id=%d value=%f label=%s (rc=%d)\n",
           custom_name,
           custom.id,
           custom.value,
           custom.label,
           custom_rc);
    serial_printf("[global-var-test] set %s => id=%d value=%f label=%s (rc=%d)\n",
                  custom_name,
                  custom.id,
                  custom.value,
                  custom.label,
                  custom_rc);
    int custom_get_rc = IPO_VAR_GET(custom_name, &custom_out);
    printf("[global-var-test] get %s => id=%d value=%f label=%s (rc=%d)\n",
           custom_name,
           custom_out.id,
           custom_out.value,
           custom_out.label,
           custom_get_rc);
    serial_printf("[global-var-test] get %s => id=%d value=%f label=%s (rc=%d)\n",
                  custom_name,
                  custom_out.id,
                  custom_out.value,
                  custom_out.label,
                  custom_get_rc);

    printf("[global-var-test] delete %s\n", name);
    serial_printf("[global-var-test] delete %s\n", name);
    int delete_rc = ipo_var_delete(name);
    printf("[global-var-test] delete %s rc=%d\n", name, delete_rc);
    serial_printf("[global-var-test] delete %s rc=%d\n", name, delete_rc);

    int after_delete = 0;
    int after_delete_rc = IPO_VAR_GET(name, &after_delete);
    printf("[global-var-test] after delete read %s => int=%d (rc=%d)\n",
           name, after_delete, after_delete_rc);
    serial_printf("[global-var-test] after delete read %s => int=%d (rc=%d)\n",
                  name, after_delete, after_delete_rc);

    return 0;
}
