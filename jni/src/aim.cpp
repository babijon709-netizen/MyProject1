     // ПРИВЯЗКА к кости (правка 05.10 #2, разбор остаточного дёрганья).
    // Адаптивная доля: при большом остатке (далеко от кости) спешим, близко —
    // замедляемся. Стабилизирует приход без рывков. При остатке < 0.5° шаг
    // режется до 0.35° через ограничитель рывка, что делает последние шаги
    // очень плавными. На подходе к цели дёрганья нет — знак шага всегда в
    // сторону кости. Как только вошли в мёртвую зону (остаток < 0.01°),
    // прицел отпускается и движется сам по себе до следующей цели.
    float errMag = sqrtf(best.yaw * best.yaw + best.pitch * best.pitch);
    float adaptiveFrac = 0.55f;
    if (errMag > 2.0f) adaptiveFrac = 0.80f;
    else if (errMag > 1.0f) adaptiveFrac = 0.70f;
    else if (errMag > 0.5f) adaptiveFrac = 0.60f;
    
    float stepYaw   = best.yaw   * adaptiveFrac;
    float stepPitch = best.pitch * adaptiveFrac;
    {
         const float maxDYaw   = 0.2f + fabsf(best.yaw)   * 0.6f;
         const float maxDPitch = 0.2f + fabsf(best.pitch) * 0.6f;
         if (stepYaw   > s_prevStepYaw   + maxDYaw)   stepYaw   = s_prevStepYaw   + maxDYaw;
         if (stepYaw   < s_prevStepYaw   - maxDYaw)   stepYaw   = s_prevStepYaw   - maxDYaw;
         if (stepPitch > s_prevStepPitch + maxDPitch) stepPitch = s_prevStepPitch + maxDPitch;
         if (stepPitch < s_prevStepPitch - maxDPitch) stepPitch = s_prevStepPitch - maxDPitch;
     }
