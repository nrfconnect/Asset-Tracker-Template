# Lesson 4, Exercise 2: Modify Main Module Business Logic

## Objective

Customize the main module to implement conditional sampling logic based on sensor readings.

## Task Description

Modify the main module to:
1. Sample temperature more frequently when hot (>25°C)
2. Skip GNSS when temperature is stable
3. Change LED color based on temperature range

## Implementation

Edit `src/modules/main/main.c` to add this logic in the environmental sample response handler.

## Example Logic

```c
if (temperature > 25.0) {
    /* Hot - sample more frequently */
    next_interval = 60;  /* 1 minute */
    led_color = RED;
} else if (temperature < 15.0) {
    /* Cold - sample normally */
    next_interval = 300;  /* 5 minutes */
    led_color = BLUE;
} else {
    /* Normal - sample less frequently */
    next_interval = 600;  /* 10 minutes */
    led_color = GREEN;
}

/* Skip GNSS if temperature hasn't changed much */
if (abs(temperature - last_temperature) < 1.0) {
    skip_location = true;
}
```

## Testing

1. Warm up the device (hold in hand)
2. Observe faster sampling
3. Cool down the device
4. Observe return to normal intervals

See `solution/` directory for complete implementation.

