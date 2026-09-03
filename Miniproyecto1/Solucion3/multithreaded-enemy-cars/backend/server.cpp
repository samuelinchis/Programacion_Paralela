#include <iostream>
#include <vector>
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

// =====================================================
// COLORES DE ENEMIGOS
//
// Ahora existen 5 hilos, uno por cada color de auto.
// Cada hilo administra su propia lista de autos activos
// de ese color (puede tener varios autos a la vez).
// =====================================================

enum EnemyColor {
    COLOR_RED = 0,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_PINK,
    COLOR_WHITE,
    NUM_COLORS
};

const char* colorNames[NUM_COLORS] = {
    "red",
    "blue",
    "green",
    "pink",
    "white"
};

// -------------------------------------------------
// Límite de autos activos POR COLOR (no global)
// -------------------------------------------------

#define MAX_ENEMIES_PER_COLOR 20


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
// 3 carriles, 2 posiciones por carril.
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

    EnemyColor color;
};


// =====================================================
// LISTAS DE ENEMIGOS POR COLOR
//
// Cada color tiene su propio vector dinámico de autos
// activos y su propio mutex. Un hilo por color se
// encarga únicamente de mover/limpiar los autos de
// su color, sin importar cuántos haya.
// =====================================================

std::vector<Enemy> colorEnemies[NUM_COLORS];

pthread_mutex_t colorMutex[NUM_COLORS] = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER
};


// =====================================================
// MUTEX PARA DIFICULTAD Y CONTADOR DE ID
// =====================================================

pthread_mutex_t difficultyMutex =
    PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t idMutex =
    PTHREAD_MUTEX_INITIALIZER;


// =====================================================
// VARIABLES DE DIFICULTAD (GLOBALES, COMPARTIDAS
// ENTRE TODOS LOS COLORES)
// =====================================================

int enemiesEvaded = 0;

float currentEnemySpeed =
    INITIAL_ENEMY_SPEED;

int currentMsToRelease =
    INITIAL_MS_TO_RELEASE;

int nextEnemyId = 1;


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
// ACTUALIZAR DIFICULTAD
// =====================================================

void updateDifficulty() {

    pthread_mutex_lock(&difficultyMutex);


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


        if (
            currentMsToRelease <
            MAX_ENEMY_MS_TO_RELEASE
        ) {

            currentMsToRelease =
                MAX_ENEMY_MS_TO_RELEASE;
        }
    }


    pthread_mutex_unlock(&difficultyMutex);
}


// =====================================================
// HILO POR COLOR
//
// Cada uno de los 5 hilos se queda para siempre con
// UN color asignado (arg apunta a su EnemyColor).
// Dentro, recorre TODOS los autos activos de ese color
// (puede haber 0, 1 o varios al mismo tiempo) y los
// mueve. Cuando un auto sale de pantalla, se cuenta
// como evadido y se elimina de la lista de ese color.
// =====================================================

void* enemyColorThread(void* arg) {

    EnemyColor color =
        *static_cast<EnemyColor*>(arg);


    while (true) {

        pthread_mutex_lock(&colorMutex[color]);


        std::vector<Enemy>& list =
            colorEnemies[color];


        for (
            size_t i = 0;
            i < list.size();
            /* incremento manual */
        ) {

            // ---------------------------------------------
            // Mover el auto
            // ---------------------------------------------

            list[i].y +=
                list[i].speed;


            // ---------------------------------------------
            // Si sale de la pantalla, se evadió
            // ---------------------------------------------

            if (
                list[i].y > GAME_HEIGHT
            ) {

                pthread_mutex_lock(&difficultyMutex);
                enemiesEvaded++;
                pthread_mutex_unlock(&difficultyMutex);

                updateDifficulty();


                // -----------------------------------------
                // Eliminar de la lista de este color
                // (swap-and-pop, orden no importa)
                // -----------------------------------------

                list[i] =
                    list.back();

                list.pop_back();

                // no incrementar i: en su lugar quedó
                // el elemento que era el último

            } else {

                i++;
            }
        }


        pthread_mutex_unlock(&colorMutex[color]);


        // -------------------------------------------------
        // Actualizar cada 50 ms
        // -------------------------------------------------

        usleep(50000);
    }


    return nullptr;
}


// =====================================================
// HILO DE GENERACIÓN DE ENEMIGOS
//
// Ya no busca "un hueco disponible" en un arreglo fijo:
// ahora elige un color al azar y, si ese color todavía
// no llegó a su límite de autos activos, le agrega uno
// nuevo a SU lista, independientemente de cuántos autos
// tengan ya los otros colores.
// =====================================================

void* spawnThread(void* arg) {

    while (true) {

        // -------------------------------------------------
        // Elegir color al azar para el nuevo auto
        // -------------------------------------------------

        EnemyColor color =
            static_cast<EnemyColor>(
                rand() % NUM_COLORS
            );


        pthread_mutex_lock(&colorMutex[color]);


        if (
            colorEnemies[color].size() <
            MAX_ENEMIES_PER_COLOR
        ) {

            Enemy enemy;


            pthread_mutex_lock(&idMutex);
            enemy.id = nextEnemyId++;
            pthread_mutex_unlock(&idMutex);


            int position =
                rand() % NUM_POSITIONS;

            enemy.x =
                enemyPositions[position];

            enemy.y =
                -100.0f;

            pthread_mutex_lock(&difficultyMutex);
            enemy.speed = currentEnemySpeed;
            pthread_mutex_unlock(&difficultyMutex);

            enemy.color =
                color;


            colorEnemies[color].push_back(enemy);


            std::cout
                << "Enemigo generado: "
                << enemy.id
                << " | Color: "
                << colorNames[color]
                << " | X: "
                << enemy.x
                << " | Velocidad: "
                << enemy.speed
                << std::endl;
        }


        pthread_mutex_unlock(&colorMutex[color]);


        int releaseTime;

        pthread_mutex_lock(&difficultyMutex);
        releaseTime = currentMsToRelease;
        pthread_mutex_unlock(&difficultyMutex);


        usleep(
            releaseTime * 1000
        );
    }


    return nullptr;
}


// =====================================================
// CREAR JSON
//
// Recorre las 5 listas (una por color) e incluye el
// campo "color" para que el frontend sepa qué sprite
// usar para cada auto.
// =====================================================

void createJson(char* json) {

    strcpy(
        json,
        "["
    );


    bool firstEnemy =
        true;


    for (
        int c = 0;
        c < NUM_COLORS;
        c++
    ) {

        pthread_mutex_lock(&colorMutex[c]);


        for (
            size_t i = 0;
            i < colorEnemies[c].size();
            i++
        ) {

            const Enemy& enemy =
                colorEnemies[c][i];

            char enemyJson[220];


            sprintf(
                enemyJson,

                "{\"id\":%d,"
                "\"x\":%.2f,"
                "\"y\":%.2f,"
                "\"speed\":%.2f,"
                "\"color\":\"%s\"}",

                enemy.id,
                enemy.x,
                enemy.y,
                enemy.speed,
                colorNames[enemy.color]
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


        pthread_mutex_unlock(&colorMutex[c]);
    }


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


    int option = 1;


    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );


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
        << "Hilos de color: "
        << NUM_COLORS
        << std::endl;

    for (
        int c = 0;
        c < NUM_COLORS;
        c++
    ) {

        std::cout
            << "  - "
            << colorNames[c]
            << " (max "
            << MAX_ENEMIES_PER_COLOR
            << " autos activos)"
            << std::endl;
    }

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


        char json[10000];


        createJson(
            json
        );


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

    srand(
        static_cast<unsigned int>(
            time(nullptr)
        )
    );


    std::cout
        << "Generador de enemigos iniciado (5 hilos por color)"
        << std::endl;


    // -------------------------------------------------
    // Crear un hilo por color (5 en total).
    //
    // Cada hilo se queda con su EnemyColor fijo durante
    // toda la ejecución; los autos van y vienen dentro
    // de la lista de ese color, pero el hilo en sí
    // siempre representa el mismo color.
    // -------------------------------------------------

    pthread_t colorThreads[NUM_COLORS];

    static EnemyColor colorIds[NUM_COLORS] = {
        COLOR_RED,
        COLOR_BLUE,
        COLOR_GREEN,
        COLOR_PINK,
        COLOR_WHITE
    };

    for (
        int c = 0;
        c < NUM_COLORS;
        c++
    ) {

        pthread_create(
            &colorThreads[c],

            nullptr,

            enemyColorThread,

            &colorIds[c]
        );
    }


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
    // Esperar hilos
    // -------------------------------------------------

    for (
        int c = 0;
        c < NUM_COLORS;
        c++
    ) {

        pthread_join(
            colorThreads[c],
            nullptr
        );
    }


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