#pragma once

// Avvia l'unico slave Modbus RTU del simulatore sulla porta seriale (UART1).
// Legge i parametri seriali e la modalita' fisica da g_cfg (config ridotta,
// monoporta). Espone tutto lo store (holding/input/coil/discrete): e' il master
// remoto (il gateway sotto test) a chiedere solo i registri che gli servono.
void mb_slave_rtu_start(void);
