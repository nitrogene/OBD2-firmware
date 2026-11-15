#ifndef CLI_COMMANDS_H
#define CLI_COMMANDS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register MODE_BRINGUP CLI commands and start USB-C CDC console REPL.
 *
 * @return ESP_OK on success.
 */
esp_err_t cli_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_COMMANDS_H */
