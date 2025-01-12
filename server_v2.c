#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024

// Function to handle communication with a single client
void *handle_client(void *client_socket) {
    int client_fd = *(int *)client_socket;
    free(client_socket); // Free memory allocated for the socket descriptor
    char buffer[BUFFER_SIZE] = {0};

    printf("Client thread started.\n");

    // Exchange messages in a loop
    while (1) {
        // Read message from the client
        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE);
        if (bytes_read <= 0) {
            printf("Client disconnected.\n");
            break;
        }

        printf("Message from client: %s\n", buffer);

        // Send a response back to the client
        send(client_fd, buffer, bytes_read, 0);
        printf("Message echoed to client: %s\n", buffer);
    }

    // Close the client connection
    close(client_fd);
    return NULL;
}

int main() {
    int server_fd, new_client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the port
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", PORT);

    while (1) {
        // Accept a new client connection
        if ((new_client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
            perror("Accept failed");
            continue;
        }

        printf("Client connected.\n");

        // Allocate memory for the client socket descriptor
        int *client_socket = malloc(sizeof(int));
        *client_socket = new_client_fd;

        // Create a new thread to handle the client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_socket) != 0) {
            perror("Failed to create thread");
            close(new_client_fd);
            free(client_socket);
        } else {
            pthread_detach(thread_id); // Detach the thread to handle cleanup automatically
        }
    }

    // Close the server socket
    close(server_fd);

    return 0;
}