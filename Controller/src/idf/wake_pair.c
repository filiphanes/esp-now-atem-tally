#include "wake_pair.h"

#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "wake";

esp_err_t wake_pair_init(wake_pair_t *pair)
{
    pair->rx_fd = -1;
    pair->tx_fd = -1;

    int rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx < 0) {
        ESP_LOGW(TAG, "wakeup socket failed: errno %d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001u);   // 127.0.0.1
    addr.sin_port = 0;                            // any free port
    socklen_t addr_len = sizeof(addr);
    if (bind(rx, (const struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        getsockname(rx, (struct sockaddr *)&addr, &addr_len) < 0) {
        ESP_LOGW(TAG, "wakeup bind failed: errno %d", errno);
        close(rx);
        return ESP_FAIL;
    }

    int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx < 0 ||
        connect(tx, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "wakeup connect failed: errno %d", errno);
        if (tx >= 0) {
            close(tx);
        }
        close(rx);
        return ESP_FAIL;
    }

    // Neither end may block: the poke runs in whatever task produced the
    // event, the drain in the task that was sleeping.
    fcntl(rx, F_SETFL, fcntl(rx, F_GETFL, 0) | O_NONBLOCK);
    fcntl(tx, F_SETFL, fcntl(tx, F_GETFL, 0) | O_NONBLOCK);
    // Publish only once both ends are up, so a racing poke never touches a
    // half-initialised socket.
    pair->rx_fd = rx;
    pair->tx_fd = tx;
    return ESP_OK;
}

void wake_pair_poke(const wake_pair_t *pair)
{
    if (pair->tx_fd >= 0) {
        char byte = 0;
        send(pair->tx_fd, &byte, 1, MSG_DONTWAIT);
    }
}

void wake_pair_drain(const wake_pair_t *pair)
{
    char scratch[16];
    while (pair->rx_fd >= 0 &&
           recv(pair->rx_fd, scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {
    }
}
