#include "define.h"
#include "network.h"
#include "utils.h"

int timedOut = 0;
int chatSocket = -1;

void handleTimeout(int sig) {
    logWarning("Timeout (90s): Disconnected from discussion.");
    timedOut = 1;
    close(chatSocket);
}

void broadcast(Customer* customer, char* msg) {
    sendto(customer->bcast.fd, msg, strlen(msg), 0, (struct sockaddr*)&customer->bcast.addr,
           sizeof(customer->bcast.addr));
}

int initBroadcastCustomer(Customer* customer) {
    logInfo("Initializing broadcast for customer.");
    int bcfd = initBroadcast(&customer->bcast.addr);
    if (bcfd < 0) return bcfd;
    customer->bcast.fd = bcfd;

    broadcast(customer, REG_REQ_MSG);
    logInfo("Broadcast for customer initialized.");
}

void loadFoodNames(Customer* customer) {
    logInfo("Loading food names.");
    cJSON* root = loadJSON();
    if (root == NULL) return;

    int foodSize = cJSON_GetArraySize(root);
    customer->foodSize = foodSize;

    cJSON* item = NULL;
    int index = 0;
    cJSON_ArrayForEach(item, root) {
        customer->foods[index] = strdup(item->string);
        index++;
    }

    cJSON_Delete(root);
    logInfo("Food names loaded.");
}

void printMenuSummary(const Customer* customer) {
    logInfo("Printing menu summary.");
    logLamination();
    logNormal("Menu:\n");
    char Food[BUF_MSG] = {'\0'};
    for (int i = 0; i < customer->foodSize; i++) {
        memset(Food, STRING_END, BUF_MSG);
        sprintf(Food, "%d. %s", i + 1, customer->foods[i]);
        logNormal(Food);
    }
    logLamination();
    logInfo("Menu summary printed.");
}

void initCustomer(Customer* customer, char* port) {
    logInfo("Initializing customer.");
    initBroadcastCustomer(customer);

    customer->tcpPort = atoi(port);
    initTCP(&customer->tcpPort);

    loadFoodNames(customer);

    getInput(STDIN_FILENO, "Enter your name: ", customer->name, BUF_NAME);

    customer->restaurantSize = 0;

    logInfo("Customer initialized.");
}

char* generateFoodRequest(Customer* customer, char* foodName) {
    char msg[BUF_MSG] = {STRING_END};

    // clang-format off
    // foodName|customerPort
    snprintf(msg, BUF_MSG, "%s%c%d", 
             foodName, BCAST_IN_DELIM, 
             customer->tcpPort);
    // clang-format on

    return strdup(msg);
}

void orderFood(Customer* customer, char* msg) {
    char food[BUF_NAME] = {STRING_END};
    getInput(STDIN_FILENO, "Enter food name: ", food, BUF_NAME);
    char portStr[BUF_CLI] = {STRING_END};
    getInput(STDIN_FILENO, "Enter restaurant port: ", portStr, BUF_CLI);
    unsigned short port = atoi(portStr);

    int fd = connectServer(port);
    char* req = generateFoodRequest(customer, food);
    send(fd, req, strlen(req), 0);

    logInfo("Order sent.");
}

void printHelp() {
    logLamination();
    logNormal("Commands:\n");
    logNormal("help: print this help.\n");
    logNormal("menu: print the menu summary.\n");
    logNormal("order: order food.\n");
    logNormal("exit: exit the program.\n");
    logLamination();
}

void printRestaurants(Customer* customer) {
    logLamination();
    logNormal("Restaurants:\n");
    for (int i = 0; i < customer->restaurantSize; i++) {
        char msg[BUF_MSG] = {STRING_END};
        sprintf(msg, "%d. %s - %d\n", i + 1, customer->restaurants[i].name,
                customer->restaurants[i].port);
        logNormal(msg);
    }
    logLamination();
}

void cli(Customer* customer, FdSet* fdset) {
    char msg[BUF_MSG] = {STRING_END};

    getInput(STDIN_FILENO, "Enter command: ", msg, BUF_MSG);

    if (!strcmp(msg, "help"))
        printHelp();
    else if (!strcmp(msg, "menu"))
        printMenuSummary(customer);
    else if (!strcmp(msg, "order"))
        orderFood(customer, msg);
    else if (!strcmp(msg, "restaurants")) 
        printRestaurants(customer);
    else if (!strcmp(msg, "exit")) {
        logInfo("Exiting.");
        exit(EXIT_SUCCESS);
    } else
        logError("Invalid command.");
}

void addRestaurant(Customer* customer, char* name, int port) {
    strcpy(customer->restaurants[customer->restaurantSize].name, name);
    customer->restaurants[customer->restaurantSize].port = port;
    customer->restaurantSize++;
}

void removeRestaurant(Customer* customer, char* name) {
    for (int i = 0; i < customer->restaurantSize; i++) {
        if (!strcmp(customer->restaurants[i].name, name)) {
            for (int j = i; j < customer->restaurantSize - 1; j++) {
                customer->restaurants[j] = customer->restaurants[j + 1];
            }
            customer->restaurantSize--;
            break;
        }
    }
}

void UDPHandlerChat(char* msgBuf, Customer* customer) {
    char* name;
    int port;
    BroadcastType type;
    deserializer(msgBuf, name, &port, &type);
    if (!strcmp(customer->name, name) && customer->tcpPort != port) {
        int fd = connectServer(port);
        send(fd, TERMINATE_MSG, strlen(TERMINATE_MSG), 0);
        return;
    }
    if (type == RESTAURANT) addRestaurant(customer, name, port);
}

void UDPHandler(Customer* customer, FdSet* fdset) {
    char msgBuf[BUF_MSG] = {STRING_END};
    int recvCount = recvfrom(customer->bcast.fd, msgBuf, BUF_MSG, 0, NULL, NULL);
    if (recvCount == 0) {
        logError("Error receiving broadcast.");
        return;
    }

    if (!strcmp(msgBuf, REG_REQ_MSG)) broadcast(customer, serializerCustomer(customer, REGISTERING));

    char* cmd = strtok(msgBuf, REQ_DELIM);
    if (cmd != NULL) {
        if (!strcmp(cmd, OPEN_REST_MSG)) {
            char* name = strtok(NULL, REQ_DELIM);
            int port = atoi(strtok(NULL, REQ_DELIM));
            addRestaurant(customer, name, port);
        } else if (!strcmp(cmd, CLOSE_REST_MSG)) {
            char* name = strtok(NULL, REQ_DELIM);
            removeRestaurant(customer, name);
        }
    } else {
        UDPHandlerChat(msgBuf, customer);
    }
}

void newConnectionHandler(int fd, Supplier* supplier, FdSet* fdset) {
    logInfo("New connection request.");
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int newfd = accept(fd, (struct sockaddr*)&addr, &addrlen);
    if (newfd < 0) {
        logError("Error accepting new connection.");
        return;
    }
    FD_SETTER(newfd, fdset);
    logInfo("New connection accepted.");
}

void chatHandler(int fd, char* msgBuf, Customer* customer, FdSet* fdset) {
    int recvCount = recv(fd, msgBuf, BUF_MSG, 0);
    if (recvCount == 0) {
        logInfo("Connection closed.");
        close(fd);
        FD_CLRER(fd, fdset);
        return;
    }

    char* cmd = strtok(msgBuf, REQ_DELIM);
    if (cmd != NULL) {
        if (!strcmp(cmd, TERMINATE_MSG)) {
            logInfo("Duplication in username.");
            close(fd);
            FD_CLRER(fd, fdset);
        }
    } else {
        logNormal(msgBuf);
    }
}

void interface(Customer* customer) {
    char msgBuf[BUF_MSG] = {STRING_END};

    FdSet fdset;
    InitFdSet(&fdset, customer->bcast.fd);

    while (1) {
        cliPrompt();
        memset(msgBuf, STRING_END, BUF_MSG);
        fdset.working = fdset.master;
        select(fdset.max + 1, &fdset.working, NULL, NULL, NULL);

        for (int i = 0; i <= fdset.max; ++i) {
            if (!FD_ISSET(i, &fdset.working)) continue;

            // this if is for having a clean interface when receiving messages
            if (i != STDIN_FILENO) write(STDOUT_FILENO, CLEAR_LINE_ANSI, CLEAR_LINE_LEN);

            if (i == STDIN_FILENO)
                cli(customer, &fdset);
            else if (i == customer->bcast.fd)
                UDPHandler(customer, &fdset);
            else if (i == customer->tcpPort)
                newConnectionHandler(i, customer, &fdset);
            else
                chatHandler(i, msgBuf, customer, &fdset);
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        logError("Usage: ./customer <port>");
        exit(EXIT_FAILURE);
    }

    Customer customer;
    initCustomer(&customer, argv[1]);

    struct sigaction sigact = {.sa_handler = handleTimeout, .sa_flags = SA_RESTART};
    sigaction(SIGALRM, &sigact, NULL);

    interface(&customer);
}