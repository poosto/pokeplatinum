# Mach-O assembly has special rules about label names and arithmetic
# This workaround wraps the raw bytes assembled using arm-none-eabi-gcc into a Mach-O binary

.text

.global _gTrainerAITable

_gTrainerAITable:
    .incbin "trainer_ai_script.bin"


