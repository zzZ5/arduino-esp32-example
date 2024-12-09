#include <Arduino.h>

// put function declarations here:
int myFunction(int, int);

void setup() {
  // put your setup code here, to run once:
  int result = myFunction(2, 3);
  printf("Result: %d\n", result);
}

void loop() {

}

// put function definitions here:
int myFunction(int x, int y) {
  return x + y;
}