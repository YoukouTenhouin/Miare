# Specify crash recovery and corruption behavior

Type: grilling
Status: open
Blocked by: 03, 04, 05, 07

## Question

For every interruption point and durable file state, what must open and recovery do; how are incomplete commits distinguished from corruption or authentication failure; what data-loss boundary is promised; and when must the library fail closed rather than repair, salvage, or continue read-only?
