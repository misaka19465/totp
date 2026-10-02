/**
 * @file    fake_plug_detect.h
 * @brief   主机侧测试替身：手动置位/清除插头唤醒事件。
 */
#ifndef FAKE_PLUG_DETECT_H
#define FAKE_PLUG_DETECT_H

/** 清除事件（plug_detect_init 亦会清除） */
void fake_plug_detect_reset(void);

/** 模拟一个 PA10 下降沿（插头唤醒前导码的第一个边沿） */
void fake_plug_detect_set_pending(void);

#endif /* FAKE_PLUG_DETECT_H */
