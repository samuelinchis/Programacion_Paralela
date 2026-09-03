#include <iostream>
#include <unistd.h>
#include <pthread.h>
#include <cstdlib>
#include <ctime>
#include <cstring>
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
// ENEMIGOS
// =====================================================

Enemy enemies[MAX_ENEMIES];


// =====================================================
// MUTEX
// =====================================================

pthread_mutex_t mutex =
    PTHREAD_MUTEX_INITIALIZER;


// =====================================================
// VARIABLES DE DIFICULTAD
// =====================================================

int enemiesEvaded = 0;

float currentEnemySpeed =
    INITIAL_ENEMY_SPEED;

int currentMsToRelease =
    INITIAL_MS_TO_RELEASE;


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

    // -------------------------------------------------
    // Seleccionar una de las 6 posiciones
    // -------------------------------------------------

    int position =
        rand() % NUM_POSITIONS;


    enemy->x =
        enemyPositions[position];


    // -------------------------------------------------
    // El enemigo aparece ligeramente por encima
    // de la pantalla
    // -------------------------------------------------

    enemy->y =
        -100.0f;


    // -------------------------------------------------
    // Velocidad actual
    //
    // La velocidad aumenta según los autos evadidos.
    // -------------------------------------------------

    enemy->speed =
        currentEnemySpeed;


    enemy->active =
        true;
}


// =====================================================
// BUSCAR UN HUECO DISPONIBLE
// =====================================================

int findInactiveEnemy() {

    for (
        int i = 0;
        i < MAX_ENEMIES;
        i++
    ) {

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

    // -------------------------------------------------
    // Aumentar velocidad cada 15 autos evadidos
    // -------------------------------------------------

    if (
        enemiesEvaded > 0 &&
        enemiesEvaded % SPEED_INCREASE_INTERVAL == 0
    ) {

        currentEnemySpeed += 1.0f;


        if (
            currentEnemySpeed >
            MAX_ENEMY_SPEED
        ) {

            currentEnemySpeed =
                MAX_ENEMY_SPEED;
        }
    }


    // -------------------------------------------------
    // Aumentar frecuencia de aparición cada
    // 30 autos evadidos
    // -------------------------------------------------

    if (
        enemiesEvaded > 0 &&
        enemiesEvaded % SPAWN_RATE_INCREASE_INTERVAL == 0
    ) {

        currentMsToRelease -=
            SPAWN_RATE_DECREASE;


        // -------------------------------------------------
        // Nunca permitir menos de 250 ms
        // -------------------------------------------------

        if (
            currentMsToRelease <
            MAX_ENEMY_MS_TO_RELEASE
        ) {

            currentMsToRelease =
                MAX_ENEMY_MS_TO_RELEASE;
        }
    }
}


// =====================================================
// HILO DE CADA ENEMIGO
// =====================================================

void* enemyThread(void* arg) {

    Enemy* enemy =
        static_cast<Enemy*>(arg);


    while (true) {

        pthread_mutex_lock(&mutex);


        // -------------------------------------------------
        // Hacer un for para que el hilo recorra todos los carros
        // -------------------------------------------------

        for (int i = 0; i < MAX_ENEMIES; i++) {

            if (enemies[i].active) {

                enemies[i].y += enemies[i].speed;

            // -------------------------------------------------
            // Si sale de la pantalla
            // -------------------------------------------------

                if (enemies[i].y > GAME_HEIGHT) {

                // ---------------------------------------------
                // El jugador evitó el enemigo
                // ---------------------------------------------

                    enemiesEvaded++;


                // ---------------------------------------------
                // Actualizar dificultad
                // ---------------------------------------------

                    updateDifficulty();


                // ---------------------------------------------
                // Desactivar enemigo
                // ---------------------------------------------

                    enemies[i].active = false;
                }
            }
        }


        pthread_mutex_unlock(&mutex);


        // -------------------------------------------------
        // Actualizar cada 50 ms
        // -------------------------------------------------

        usleep(50000);
    }


    return nullptr;
}


// =====================================================
// HILO DE GENERACIÓN DE ENEMIGOS
// =====================================================

void* spawnThread(void* arg) {

    while (true) {

        pthread_mutex_lock(&mutex);


        // -------------------------------------------------
        // Buscar un hilo/enemigo disponible
        // -------------------------------------------------

        int enemyIndex =
            findInactiveEnemy();


        if (
            enemyIndex != -1
        ) {

            // ---------------------------------------------
            // Generar nuevo enemigo
            // ---------------------------------------------

            generateEnemy(
                &enemies[enemyIndex]
            );


            std::cout
                << "Enemigo generado: "
                << enemies[enemyIndex].id
                << " | X: "
                << enemies[enemyIndex].x
                << " | Velocidad: "
                << enemies[enemyIndex].speed
                << " | Intervalo: "
                << currentMsToRelease
                << " ms"
                << std::endl;
        }


        int releaseTime =
            currentMsToRelease;


        pthread_mutex_unlock(&mutex);


        // -------------------------------------------------
        // Esperar antes del siguiente enemigo
        //
        // Inicialmente:
        //
        // 500 ms
        //
        // Después de 30 evadidos:
        //
        // 450 ms
        //
        // Después:
        //
        // 400 ms
        //
        // ...
        //
        // Mínimo:
        //
        // 250 ms
        // -------------------------------------------------

        usleep(
            releaseTime * 1000
        );
    }


    return nullptr;
}


// =====================================================
// CREAR JSON
// =====================================================

void createJson(char* json) {

    strcpy(
        json,
        "["
    );


    pthread_mutex_lock(&mutex);


    bool firstEnemy =
        true;


    for (
        int i = 0;
        i < MAX_ENEMIES;
        i++
    ) {

        // -------------------------------------------------
        // Solo enviar enemigos activos
        // -------------------------------------------------

        if (
            !enemies[i].active
        ) {

            continue;
        }


        char enemyJson[200];


        sprintf(
            enemyJson,

            "{\"id\":%d,"
            "\"x\":%.2f,"
            "\"y\":%.2f,"
            "\"speed\":%.2f}",

            enemies[i].id,
            enemies[i].x,
            enemies[i].y,
            enemies[i].speed
        );


        if (!firstEnemy) {

            strcat(
                json,
                ","
            );
        }


        strcat(
            json,
            enemyJson
        );


        firstEnemy =
            false;
    }


    pthread_mutex_unlock(&mutex);


    strcat(
        json,
        "]"
    );
}


// =====================================================
// SERVIDOR HTTP
// =====================================================

void* serverThread(void* arg) {

    int serverSocket =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (
        serverSocket < 0
    ) {

        std::cerr
            << "Error creando socket"
            << std::endl;


        return nullptr;
    }


    // -------------------------------------------------
    // Permitir reutilizar puerto
    // -------------------------------------------------

    int option = 1;


    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );


    // -------------------------------------------------
    // Configurar servidor
    // -------------------------------------------------

    sockaddr_in serverAddress;


    memset(
        &serverAddress,
        0,
        sizeof(serverAddress)
    );


    serverAddress.sin_family =
        AF_INET;


    serverAddress.sin_addr.s_addr =
        INADDR_ANY;


    serverAddress.sin_port =
        htons(PORT);


    // -------------------------------------------------
    // BIND
    // -------------------------------------------------

    if (
        bind(
            serverSocket,
            (sockaddr*)&serverAddress,
            sizeof(serverAddress)
        ) < 0
    ) {

        std::cerr
            << "Error haciendo bind al puerto "
            << PORT
            << std::endl;


        close(
            serverSocket
        );


        return nullptr;
    }


    // -------------------------------------------------
    // LISTEN
    // -------------------------------------------------

    if (
        listen(
            serverSocket,
            10
        ) < 0
    ) {

        std::cerr
            << "Error iniciando servidor"
            << std::endl;


        close(
            serverSocket
        );


        return nullptr;
    }


    // -------------------------------------------------
    // INFORMACIÓN
    // -------------------------------------------------

    std::cout
        << "================================"
        << std::endl;


    std::cout
        << "Servidor HTTP iniciado"
        << std::endl;


    std::cout
        << "http://localhost:"
        << PORT
        << std::endl;


    std::cout
        << "================================"
        << std::endl;


    std::cout
        << "Enemigos máximos: "
        << MAX_ENEMIES
        << std::endl;


    std::cout
        << "Posiciones: "
        << NUM_POSITIONS
        << std::endl;


    std::cout
        << "Velocidad inicial: "
        << INITIAL_ENEMY_SPEED
        << std::endl;


    std::cout
        << "Velocidad máxima: "
        << MAX_ENEMY_SPEED
        << std::endl;


    std::cout
        << "Tiempo inicial de aparición: "
        << INITIAL_MS_TO_RELEASE
        << " ms"
        << std::endl;


    std::cout
        << "Tiempo mínimo de aparición: "
        << MAX_ENEMY_MS_TO_RELEASE
        << " ms"
        << std::endl;


    std::cout
        << "================================"
        << std::endl;


    // -------------------------------------------------
    // MOSTRAR POSICIONES
    // -------------------------------------------------

    std::cout
        << "Posiciones X:"
        << std::endl;


    for (
        int i = 0;
        i < NUM_POSITIONS;
        i++
    ) {

        std::cout
            << enemyPositions[i];


        if (
            i < NUM_POSITIONS - 1
        ) {

            std::cout
                << ", ";
        }
    }


    std::cout
        << std::endl;


    std::cout
        << "================================"
        << std::endl;


    // -------------------------------------------------
    // ACEPTAR CLIENTES
    // -------------------------------------------------

    while (true) {

        sockaddr_in clientAddress;


        socklen_t clientLength =
            sizeof(clientAddress);


        int clientSocket =
            accept(
                serverSocket,
                (sockaddr*)&clientAddress,
                &clientLength
            );


        if (
            clientSocket < 0
        ) {

            continue;
        }


        // -------------------------------------------------
        // RECIBIR REQUEST
        // -------------------------------------------------

        char request[2048];


        memset(
            request,
            0,
            sizeof(request)
        );


        recv(
            clientSocket,
            request,
            sizeof(request) - 1,
            0
        );


        // -------------------------------------------------
        // CREAR JSON
        // -------------------------------------------------

        char json[10000];


        createJson(
            json
        );


        // -------------------------------------------------
        // RESPUESTA HTTP
        // -------------------------------------------------

        char response[12000];


        sprintf(
            response,

            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",

            strlen(json),
            json
        );


        // -------------------------------------------------
        // ENVIAR
        // -------------------------------------------------

        send(
            clientSocket,
            response,
            strlen(response),
            0
        );


        close(
            clientSocket
        );
    }


    close(
        serverSocket
    );


    return nullptr;
}


// =====================================================
// MAIN
// =====================================================

int main() {

    // -------------------------------------------------
    // Inicializar números aleatorios
    // -------------------------------------------------

    srand(
        static_cast<unsigned int>(
            time(nullptr)
        )
    );


    std::cout
        << "Generador de enemigos iniciado"
        << std::endl;


    // -------------------------------------------------
    // Inicializar enemigos
    //
    // Los hilos existen desde el inicio,
    // pero los enemigos comienzan inactivos.
    // -------------------------------------------------

    for (
        int i = 0;
        i < MAX_ENEMIES;
        i++
    ) {

        enemies[i].id =
            i + 1;


        enemies[i].x =
            0.0f;


        enemies[i].y =
            0.0f;


        enemies[i].speed =
            INITIAL_ENEMY_SPEED;


        enemies[i].active =
            false;
    }


    // -------------------------------------------------
    // Crear un solo hilo para todos los enemigos
    //
    // Un pthread para todos los enemigos
    // -------------------------------------------------

    pthread_t enemiesThread;


    pthread_create(&enemiesThread, nullptr, enemyThread, nullptr);


    // -------------------------------------------------
    // Crear hilo de generación
    // -------------------------------------------------

    pthread_t spawner;


    pthread_create(
        &spawner,

        nullptr,

        spawnThread,

        nullptr
    );


    // -------------------------------------------------
    // Crear servidor
    // -------------------------------------------------

    pthread_t server;


    pthread_create(
        &server,

        nullptr,

        serverThread,

        nullptr
    );


    // -------------------------------------------------
    // Esperar ahora al unico hilo del enemigo
    // -------------------------------------------------

    pthread_join(enemiesThread, nullptr);


    pthread_join(
        spawner,
        nullptr
    );


    pthread_join(
        server,
        nullptr
    );


    return 0;
}