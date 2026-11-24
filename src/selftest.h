// Autotest de los módulos puros (protocolo Timemore y motor de caudal).
// En el firmware se lanza enviando 'X' por el monitor serie. No usa Arduino: el mismo
// código compila en un PC con cualquier compilador C++11 si algún día se quiere ejecutar allí.
#pragma once

// print: función que escribe una línea (sin salto final). Devuelve el número de fallos.
int runSelfTests(void (*print)(const char* line));
