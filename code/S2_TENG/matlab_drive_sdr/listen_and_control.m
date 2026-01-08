% PlutoSDR 触发监听 + FFT + 电线图 + 实时频率显示 + 串口翻转发送

% ==================== 参数设置 ====================
sample_rate = 5e6;             % 采样率 10 MHz
center_freq = 428e6;            % 中心频率 432 MHz
samples_per_frame = 4096 * 8;   % 每帧采样点数
gain = 40;                      % 增益
trigger_level = 0.2;             % 幅度触发门限
min_peak_dB = 30;               % 峰值门限

% 串口参数（修改 ESP32 端口号）
esp32_port = "COM12";           
baud_rate = 115200;             
esp32 = serialport(esp32_port, baud_rate);
disp("已连接到 ESP32");

% ==================== 初始化接收器 ====================
rx = sdrrx('Pluto', ...
    'CenterFrequency', center_freq, ...
    'BasebandSampleRate', sample_rate, ...
    'SamplesPerFrame', samples_per_frame, ...
    'GainSource', 'Manual', ...
    'Gain', gain, ...
    'OutputDataType', 'double');

disp('开始监听（触发 + FFT + 频率绘图 + 串口翻转输出）...');

% ==================== 初始化频率图 ====================
hFig2 = figure('Name', 'Freqency Time Scatter', 'NumberTitle', 'off');
hAx2 = axes('Parent', hFig2);
hold(hAx2, 'on');
grid(hAx2, 'on');
xlabel(hAx2, 'Time / s');
ylabel(hAx2, 'Freqency / MHz');
ylim(hAx2, [420 440]);
title(hAx2, 'Freqency Time');
scatterPlot = animatedline(hAx2, 'Marker', 'o', 'LineStyle', '-', 'Color', 'b');

% 添加实时频率显示文本
hFreqText = text(hAx2, 0.02, 0.95, 'Freqency: -- MHz', ...
                 'Units', 'normalized', ...
                 'FontSize', 12, ...
                 'FontWeight', 'bold', ...
                 'Color', 'blue');

% ==================== 初始化变量 ====================
start_time = datetime('now');
state = 0;   % 维护当前状态，0=灭，1=亮

% ==================== 主循环 ====================
while ishandle(hFig2)
    rxData = rx();
    amp = abs(rxData);

    % 查找触发点
    trigger_idx = find(amp(2:end) >= trigger_level & amp(1:end-1) < trigger_level, 1);

    if ~isempty(trigger_idx)
        % 截取触发波形
        win_size = samples_per_frame;
        center = round(win_size / 2);
        start_idx = max(trigger_idx - center, 1);
        end_idx = min(start_idx + win_size - 1, length(rxData));
        display_data = zeros(win_size, 1);
        display_data(1:(end_idx-start_idx+1)) = rxData(start_idx:end_idx);

        % 加窗
        window = blackman(win_size);
        display_data = display_data .* window;

        % 频谱分析
        Nfft = samples_per_frame;
        spectrum = fftshift(fft(display_data, Nfft));
        magSpec = abs(spectrum);
        magSpec(magSpec == 0) = eps;  % 防止 log(0)
        dBSpec = 20 * log10(magSpec);
        dBSpec(dBSpec < -120) = -120;

        [peakVal, idxMax] = max(dBSpec);
        freqVec = linspace(-sample_rate/2, sample_rate/2, Nfft);
        freqEstimate = freqVec(idxMax) + center_freq;

        elapsedTime = seconds(datetime('now') - start_time); % 运行时间（秒）

        % ============ 翻转并发送 ============
        state = 1 - state;   % 0→1 或 1→0
        writeline(esp32, string(state));  

        fprintf('事件: 峰值 %.2f dB, 频率 %.3f MHz -> 翻转并发送 %d\n', ...
                peakVal, freqEstimate/1e6, state);

        % 添加点到电线图
        addpoints(scatterPlot, elapsedTime, freqEstimate/1e6);

        % 保留最近60秒的数据点
        [xData, yData] = getpoints(scatterPlot);
        valid_idx = xData >= elapsedTime - 30;
        clearpoints(scatterPlot);
        addpoints(scatterPlot, xData(valid_idx), yData(valid_idx));

        % 更新 X 轴范围为最近 60 秒
        xlim(hAx2, [max(0, elapsedTime - 60), elapsedTime]);

        % 更新实时频率文本
        set(hFreqText, 'String', sprintf('Freqency: %.3f MHz', freqEstimate/1e6));
        drawnow limitrate;
    end
end

% ==================== 释放资源 ====================
release(rx);
clear esp32;
disp('监听结束');
