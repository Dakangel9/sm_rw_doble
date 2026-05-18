CC = gcc
CFLAGS = -O2 -IC:/mingw64/include/SDL2 -IC:/mingw64/include -Isrc -I.
LDFLAGS = -LC:/mingw64/lib -lmingw32 -lSDL2main -lSDL2 -lopengl32 -lole32 -ldxguid -luuid

# Listar de forma explícita o mediante comodines todos los archivos fuente
SRCS = $(wildcard src/*.c) $(wildcard src/snes/*.c) third_party/gl_core/gl_core_3_1.c src/platform/win32/volume_control.c

# Transformar la lista de archivos .c en archivos objeto .o
OBJS = $(SRCS:.c=.o)
TARGET = sm_rew_doble.exe

all: $(TARGET)

# Regla para enlazar los archivos objeto y generar el ejecutable final
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Regla genérica para compilar archivos individuales .c en .o de forma aislada
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Regla de limpieza adaptada a la consola de comandos de Windows (cmd)
clean:
	@echo Limpiando archivos objeto antiguos...
	-del /q /s src\*.o src\snes\*.o third_party\gl_core\*.o src\platform\win32\*.o $(TARGET) 2>nul