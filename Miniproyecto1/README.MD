Cada carpeta corresponde a un diseño distinto y contiene el codigo fuente así como los archivos docker-compose.yml para levantar los servicios de la aplicacion con docker compose.

Solución 1 corresponde a --> Diseño 1: Hilos Independientes
En este diseño, cada vehículo enemigo tiene su propio hilo de ejecución. Cada hilo es
responsable de actualizar únicamente un vehículo.

Solución 2 corresponde a --> Diseño 2: Hilo único de Actualización
En esta diseñó existe un solo hilo encargado de actualizar todos los vehículos.

Solución 3 corresponde a --> Diseño 3: Hilo para cada tipo de vehículo
En esta diseñó existe un solo hilo encargado de actualizar todos los vehículos de un tipo,
rojos, verdes, negros etc.

Solución 4 corresponde a --> Diseño 4: Hilo asíncrono para vehículos
En este diseño existe un grupo fijo de hilos que ejecutan tareas. Los vehículos no tienen un
hilo permanente (evalúe una implementación sin websockets). Los hilos toman las
solicitudes desde una cola.
