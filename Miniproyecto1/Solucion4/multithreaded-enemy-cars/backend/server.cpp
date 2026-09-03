#include <iostream>
#include <unistd.h>
#include <pthread.h>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <queue>
#include <atomic>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 5000

// =====================================================
// CONFIGURACIÓN BASADA EN config.js
// =====================================================

#define GAME_HEIGHT 840

#define LANES 3

#define MAX_ENEMIES 20

// Velocidad
#define INITIAL_ENEMY_SPEED 2.0f
#define MAX_ENEMY_SPEED 9.0f

// Aparición de enemigos
#define INITIAL_MS_TO_RELEASE 500
#define MAX_ENEMY_MS_TO_RELEASE 250

// Progresión
#define SPEED_INCREASE_INTERVAL 15
#define SPAWN_RATE_INCREASE_INTERVAL 30
#define SPAWN_RATE_DECREASE 50

// =====================================================
// CONFIGURACIÓN DEL POOL DE HILOS Y DEL STREAMING
// =====================================================

// Número FIJO de hilos trabajadores. Ya no se crea un
// hilo por cada enemigo: los hilos son un recurso
// reutilizable y las "solicitudes" (tareas) son las que
// van y vienen a través de una cola compartida.
#define THREAD_POOL_SIZE 4

// Cada cuántos ms el hilo productor genera nuevas tareas
// de movimiento (equivale al "tick" de la simulación).
#define TICK_INTERVAL_MS 50

// Cada cuántos ms se envía un evento SSE a cada cliente
// conectado con el estado actual del juego.
#define SSE_INTERVAL_MS 100

// =====================================================
// 6 POSICIONES X
//
// 3 carriles
// 2 posiciones por carril
//
// Se mantienen las posiciones originales
// establecidas en el C++.
// =====================================================

#define NUM_POSITIONS 6

const float enemyPositions[NUM_POSITIONS] = {
    150.0f,
    215.0f,

    280.0f,
    345.0f,

    410.0f,
    475.0f
};

// =====================================================
// ESTRUCTURA DEL ENEMIGO
// =====================================================

struct Enemy {
    int id;
    float x;
    float y;
    float speed;
    bool active;
};

// =====================================================
// ESTRUCTURA DE TAREA (lo que viaja por la cola)
//
// enemyIndex >= 0  -> "mover este enemigo un tick"
// enemyIndex == -1 -> "intentar generar un enemigo nuevo"
//
// Esto es lo único que se encola: nunca se crea un hilo
// para esto, un trabajador del pool la recoge cuando
// queda libre.
// =====================================================

struct Task {
    int enemyIndex;
};

// =====================================================
// ESTADO COMPARTIDO DEL JUEGO
// =====================================================

Enemy enemies[MAX_ENEMIES];

// Protege el arreglo "enemies" y las variables de dificultad
pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;

int enemiesEvaded = 0;
float currentEnemySpeed = INITIAL_ENEMY_SPEED;
int currentMsToRelease = INITIAL_MS_TO_RELEASE;

// =====================================================
// COLA DE TAREAS (productor/consumidor)
//
// - El hilo "ticker" (productor) encola tareas.
// - Los hilos del pool (consumidores) las desencolan.
// - queueCond evita busy-waiting: un trabajador se
//   duerme si no hay tareas y se despierta solo cuando
//   llega una nueva.
// =====================================================

std::queue<Task> taskQueue;
pthread_mutex_t queueMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queueNotEmpty = PTHREAD_COND_INITIALIZER;

std::atomic<bool> serverRunning(true);

// =====================================================
// GENERAR NÚMERO ALEATORIO
// =====================================================

float randomFloat(float min, float max) {
    return min +
        static_cast<float>(rand()) /
        static_cast<float>(RAND_MAX) *
        (max - min);
}

// =====================================================
// GENERAR ENEMIGO
// =====================================================

void generateEnemy(Enemy* enemy) {
    int position = rand() % NUM_POSITIONS;

    enemy->x = enemyPositions[position];

    // El enemigo aparece ligeramente por encima de la pantalla
    enemy->y = -100.0f;

    // La velocidad aumenta según los autos evadidos
    enemy->speed = currentEnemySpeed;

    enemy->active = true;
}

// =====================================================
// BUSCAR UN HUECO DISPONIBLE
// =====================================================

int findInactiveEnemy() {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            return i;
        }
    }

    return -1;
}

// =====================================================
// ACTUALIZAR DIFICULTAD
// =====================================================

void updateDifficulty() {
    // Aumentar velocidad cada 15 autos evadidos
    if (enemiesEvaded > 0 && enemiesEvaded % SPEED_INCREASE_INTERVAL == 0) {
        currentEnemySpeed += 1.0f;

        if (currentEnemySpeed > MAX_ENEMY_SPEED) {
            currentEnemySpeed = MAX_ENEMY_SPEED;
        }
    }

    // Aumentar frecuencia de aparición cada 30 autos evadidos
    if (enemiesEvaded > 0 && enemiesEvaded % SPAWN_RATE_INCREASE_INTERVAL == 0) {
        currentMsToRelease -= SPAWN_RATE_DECREASE;

        // Nunca permitir menos de 250 ms
        if (currentMsToRelease < MAX_ENEMY_MS_TO_RELEASE) {
            currentMsToRelease = MAX_ENEMY_MS_TO_RELEASE;
        }
    }
}

// =====================================================
// ENCOLAR UNA TAREA
//
// Cualquier hilo (aquí siempre el ticker) puede llamar
// esto. Despierta a UN trabajador dormido, si lo hay.
// =====================================================

void enqueueTask(int enemyIndex) {
    pthread_mutex_lock(&queueMutex);

    taskQueue.push(Task{enemyIndex});

    pthread_cond_signal(&queueNotEmpty);

    pthread_mutex_unlock(&queueMutex);
}

// =====================================================
// PROCESAR TAREA: MOVER UN ENEMIGO (un tick)
//
// Esto es lo que antes vivía dentro del "while(true)" de
// enemyThread. Ahora es una función corta que un hilo del
// pool ejecuta una sola vez por tarea recibida.
// =====================================================

void processEnemyMovement(int index) {
    pthread_mutex_lock(&stateMutex);

    Enemy* enemy = &enemies[index];

    if (enemy->active) {
        enemy->y += enemy->speed;

        // Si sale de la pantalla
        if (enemy->y > GAME_HEIGHT) {
            // El jugador evitó el enemigo
            enemiesEvaded++;

            updateDifficulty();

            enemy->active = false;
        }
    }

    pthread_mutex_unlock(&stateMutex);
}

// =====================================================
// PROCESAR TAREA: GENERAR UN ENEMIGO NUEVO
//
// Equivale a una iteración del antiguo spawnThread.
// =====================================================

void processSpawnEnemy() {
    pthread_mutex_lock(&stateMutex);

    int enemyIndex = findInactiveEnemy();

    if (enemyIndex != -1) {
        generateEnemy(&enemies[enemyIndex]);

        std::cout
            << "[worker " << pthread_self() << "] Enemigo generado: "
            << enemies[enemyIndex].id
            << " | X: " << enemies[enemyIndex].x
            << " | Velocidad: " << enemies[enemyIndex].speed
            << " | Intervalo: " << currentMsToRelease << " ms"
            << std::endl;
    }

    pthread_mutex_unlock(&stateMutex);
}

// =====================================================
// HILO TRABAJADOR (WORKER) DEL POOL
//
// Este es el reemplazo directo de "un hilo por enemigo".
// Un número FIJO de estos hilos (THREAD_POOL_SIZE) vive
// durante toda la ejecución del programa. Ninguno está
// "asignado" a un enemigo en particular: simplemente
// toman la siguiente tarea disponible de la cola, la
// ejecutan, y vuelven a pedir otra.
// =====================================================

void* workerThread(void* arg) {
    while (true) {
        pthread_mutex_lock(&queueMutex);

        // Dormir mientras no haya trabajo (evita busy-waiting)
        while (taskQueue.empty() && serverRunning) {
            pthread_cond_wait(&queueNotEmpty, &queueMutex);
        }

        // Condición de apagado ordenado del servidor
        if (taskQueue.empty() && !serverRunning) {
            pthread_mutex_unlock(&queueMutex);
            break;
        }

        Task task = taskQueue.front();
        taskQueue.pop();

        pthread_mutex_unlock(&queueMutex);

        // Ejecutar la tarea SIN tener la cola bloqueada,
        // para que otros trabajadores puedan seguir sacando tareas
        if (task.enemyIndex == -1) {
            processSpawnEnemy();
        } else {
            processEnemyMovement(task.enemyIndex);
        }
    }

    return nullptr;
}

// =====================================================
// HILO PRODUCTOR ("TICKER")
//
// Sustituye a los antiguos MAX_ENEMIES hilos + el hilo de
// spawn. Cada TICK_INTERVAL_MS, en lugar de mover los
// enemigos él mismo, simplemente ENCOLA una tarea por cada
// enemigo activo (y, cuando corresponde, una tarea de
// generación). El trabajo real lo hacen los hilos del pool.
// =====================================================

void* tickerThread(void* arg) {
    int msSinceLastSpawn = 0;

    while (serverRunning) {
        pthread_mutex_lock(&stateMutex);

        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (enemies[i].active) {
                enqueueTask(i);
            }
        }

        int releaseTime = currentMsToRelease;

        pthread_mutex_unlock(&stateMutex);

        msSinceLastSpawn += TICK_INTERVAL_MS;

        if (msSinceLastSpawn >= releaseTime) {
            enqueueTask(-1); // -1 = tarea de generación de enemigo
            msSinceLastSpawn = 0;
        }

        usleep(TICK_INTERVAL_MS * 1000);
    }

    return nullptr;
}

// =====================================================
// CREAR JSON CON EL ESTADO ACTUAL
// =====================================================

void createJson(char* json) {
    strcpy(json, "[");

    pthread_mutex_lock(&stateMutex);

    bool firstEnemy = true;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        // Solo enviar enemigos activos
        if (!enemies[i].active) {
            continue;
        }

        char enemyJson[200];

        sprintf(
            enemyJson,
            "{\"id\":%d,\"x\":%.2f,\"y\":%.2f,\"speed\":%.2f}",
            enemies[i].id,
            enemies[i].x,
            enemies[i].y,
            enemies[i].speed
        );

        if (!firstEnemy) {
            strcat(json, ",");
        }

        strcat(json, enemyJson);

        firstEnemy = false;
    }

    pthread_mutex_unlock(&stateMutex);

    strcat(json, "]");
}

// =====================================================
// HILO POR CLIENTE SSE
//
// IMPORTANTE: esto NO es "un hilo por enemigo" ni compite
// con el pool de tareas. Es un hilo de I/O de larga
// duración por CONEXIÓN de navegador (necesario porque, a
// diferencia de HTTP normal, en SSE el socket se mantiene
// abierto para poder empujar eventos). El número de
// enemigos y su movimiento siguen resolviéndose
// exclusivamente por el pool + la cola de tareas; este
// hilo solo LEE el estado ya calculado y lo transmite.
// =====================================================

void* handleSseClient(void* arg) {
    int clientSocket = *static_cast<int*>(arg);
    delete static_cast<int*>(arg);

    // Leer (y descartar) la petición HTTP inicial del navegador
    char request[2048];
    memset(request, 0, sizeof(request));
    recv(clientSocket, request, sizeof(request) - 1, 0);

    // Encabezados de un stream SSE: nótese que NO hay
    // Content-Length ni "Connection: close", porque la
    // conexión debe permanecer abierta indefinidamente.
    const char* sseHeaders =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";

    if (send(clientSocket, sseHeaders, strlen(sseHeaders), MSG_NOSIGNAL) <= 0) {
        close(clientSocket);
        return nullptr;
    }

    // Mientras el servidor siga vivo y el navegador siga
    // conectado, enviar un evento con el estado del juego
    while (serverRunning) {
        char json[10000];
        createJson(json);

        char event[10200];
        snprintf(event, sizeof(event), "data: %s\n\n", json);

        ssize_t sent = send(clientSocket, event, strlen(event), MSG_NOSIGNAL);

        // send() <= 0 normalmente significa que el cliente cerró
        // la pestaña o perdió la conexión: terminamos este hilo
        if (sent <= 0) {
            break;
        }

        usleep(SSE_INTERVAL_MS * 1000);
    }

    close(clientSocket);
    return nullptr;
}

// =====================================================
// SERVIDOR HTTP / SSE
//
// El accept() sigue viviendo en un único hilo, pero por
// cada cliente que se conecta se lanza un hilo dedicado
// (handleSseClient) que se "desengancha" (detach) de
// inmediato: el accept loop no espera a que termine para
// poder seguir aceptando nuevos clientes en paralelo.
// =====================================================

void* serverThread(void* arg) {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket < 0) {
        std::cerr << "Error creando socket" << std::endl;
        return nullptr;
    }

    // Permitir reutilizar puerto
    int option = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    // Configurar servidor
    sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(PORT);

    // BIND
    if (bind(serverSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        std::cerr << "Error haciendo bind al puerto " << PORT << std::endl;
        close(serverSocket);
        return nullptr;
    }

    // LISTEN
    if (listen(serverSocket, 10) < 0) {
        std::cerr << "Error iniciando servidor" << std::endl;
        close(serverSocket);
        return nullptr;
    }

    // INFORMACIÓN
    std::cout << "================================" << std::endl;
    std::cout << "Servidor SSE iniciado (sin WebSockets)" << std::endl;
    std::cout << "http://localhost:" << PORT << std::endl;
    std::cout << "================================" << std::endl;
    std::cout << "Hilos del pool: " << THREAD_POOL_SIZE << std::endl;
    std::cout << "Tick de simulación: " << TICK_INTERVAL_MS << " ms" << std::endl;
    std::cout << "Intervalo de envío SSE: " << SSE_INTERVAL_MS << " ms" << std::endl;
    std::cout << "Enemigos máximos: " << MAX_ENEMIES << std::endl;
    std::cout << "Posiciones: " << NUM_POSITIONS << std::endl;
    std::cout << "Velocidad inicial: " << INITIAL_ENEMY_SPEED << std::endl;
    std::cout << "Velocidad máxima: " << MAX_ENEMY_SPEED << std::endl;
    std::cout << "Tiempo inicial de aparición: " << INITIAL_MS_TO_RELEASE << " ms" << std::endl;
    std::cout << "Tiempo mínimo de aparición: " << MAX_ENEMY_MS_TO_RELEASE << " ms" << std::endl;
    std::cout << "================================" << std::endl;

    // ACEPTAR CLIENTES
    while (true) {
        sockaddr_in clientAddress;
        socklen_t clientLength = sizeof(clientAddress);

        int clientSocket = accept(serverSocket, (sockaddr*)&clientAddress, &clientLength);

        if (clientSocket < 0) {
            continue;
        }

        // Cada cliente SSE obtiene su propio hilo de envío,
        // desacoplado del pool que mueve los enemigos
        int* clientSocketPtr = new int(clientSocket);

        pthread_t clientThread;
        pthread_create(&clientThread, nullptr, handleSseClient, clientSocketPtr);
        pthread_detach(clientThread);
    }

    close(serverSocket);
    return nullptr;
}

// =====================================================
// MAIN
// =====================================================

int main() {
    srand(static_cast<unsigned int>(time(nullptr)));

    std::cout << "Generador de enemigos iniciado (pool de hilos + cola de tareas)" << std::endl;

    // -------------------------------------------------
    // Inicializar enemigos
    //
    // Ya no representan "un hilo cada uno": son solo datos.
    // Empiezan todos inactivos.
    // -------------------------------------------------

    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].id = i + 1;
        enemies[i].x = 0.0f;
        enemies[i].y = 0.0f;
        enemies[i].speed = INITIAL_ENEMY_SPEED;
        enemies[i].active = false;
    }

    // -------------------------------------------------
    // Crear el pool FIJO de hilos trabajadores
    //
    // Estos THREAD_POOL_SIZE hilos existen durante toda la
    // vida del programa y son compartidos por TODOS los
    // enemigos: nunca se crea ni se destruye un hilo por
    // auto, solo se encolan/desencolan tareas.
    // -------------------------------------------------

    pthread_t workerThreads[THREAD_POOL_SIZE];

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&workerThreads[i], nullptr, workerThread, nullptr);
    }

    // -------------------------------------------------
    // Crear el hilo productor (ticker), que alimenta la
    // cola de tareas en cada paso de la simulación
    // -------------------------------------------------

    pthread_t ticker;
    pthread_create(&ticker, nullptr, tickerThread, nullptr);

    // -------------------------------------------------
    // Crear el hilo del servidor SSE
    // -------------------------------------------------

    pthread_t server;
    pthread_create(&server, nullptr, serverThread, nullptr);

    // -------------------------------------------------
    // Esperar hilos (el programa corre indefinidamente)
    // -------------------------------------------------

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(workerThreads[i], nullptr);
    }

    pthread_join(ticker, nullptr);
    pthread_join(server, nullptr);

    return 0;
}