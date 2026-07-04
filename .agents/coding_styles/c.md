# C (ESP-IDF)

- Implement the module in ESP-IDF using C-style object-oriented design, not C++.
- Represent each module as an object with an opaque handle: typedef struct xxx_t *xxx_handle_t.
- The header should expose only the handle, config, events, callbacks, and public APIs.
- Define struct xxx_t only in the .c file to store object state and resources.
- Use ESP-IDF-style APIs: xxx_create/delete/start/stop/read/write/set/get.
- Use xxx_handle_t handle as the first parameter of object methods.
- Prefer esp_err_t as the return type for public APIs.
- Use const xxx_config_t *config as create input and xxx_handle_t *ret_handle as output.
- Resources must be allocated in create and fully released in delete.
- Internal resources may include memory, GPIO, I2C, SPI, timers, tasks, queues, and mutexes.
- Protect shared state with mutexes or semaphores when accessed by multiple tasks.
- Register callbacks with xxx_register_cb(), using handle, event, and user_ctx.
- For polymorphism, use an xxx_ops_t function pointer table and put base struct as the first member.