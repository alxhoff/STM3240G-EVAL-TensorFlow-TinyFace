#include <string.h>
#include "main.h"
#include "cmsis_os.h"
#include "main_functions.h"
#include "constants.h"
#include "color.h"

#include "stm324xg_eval_camera.h"

#define CAMERA_FRAME_BUFFER               0x64000000

#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif /* __GNUC__ */

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
void StartDefaultTask(void const *argument);
void StartCameraTask(void const *argument);
void blink(void const *argument);

osThreadId defaultTaskHandle;
osThreadId cameraTaskHandle;
osThreadId blinkTaskHandle;

UART_HandleTypeDef UartHandle;

/**
  * @brief  Retargets the C library printf function to the USART.
  * @param  None
  * @retval None
  */
PUTCHAR_PROTOTYPE
{
	/* Place your implementation of fputc here */
	/* e.g. write a character to the EVAL_COM1 and Loop until the end of transmission */
	HAL_UART_Transmit(&UartHandle, (uint8_t *)&ch, 1, HAL_MAX_DELAY);

	return ch;
}

#define INPUT_IMAGE_SIZE 28
#define INPUT_BOX_THICKNESS 2
#define INPUT_IMAGE_SIZE_PIXEL 224
#define INPUT_BOX_TOP_MARGIN                                                   \
	((BSP_LCD_GetYSize() - INPUT_IMAGE_SIZE_PIXEL -                        \
	  2 * INPUT_BOX_THICKNESS) /                                           \
	 2)
#define INPUT_BOX_SIDE_MARGIN INPUT_BOX_TOP_MARGIN
#define INPUT_BOX_X_OUTER INPUT_BOX_SIDE_MARGIN
#define INPUT_BOX_X_INNER INPUT_BOX_SIDE_MARGIN + INPUT_BOX_THICKNESS
#define INPUT_BOX_Y_OUTER INPUT_BOX_SIDE_MARGIN
#define INPUT_BOX_Y_INNER INPUT_BOX_SIDE_MARGIN + INPUT_BOX_THICKNESS
#define RUN_BOX_WIDTH                                                          \
	(BSP_LCD_GetXSize() - INPUT_IMAGE_SIZE_PIXEL -                         \
	 3 * INPUT_BOX_TOP_MARGIN - 2 * INPUT_BOX_THICKNESS)
#define RUN_BOX_X                                                              \
	(INPUT_BOX_SIDE_MARGIN * 2 + INPUT_BOX_THICKNESS * 2 +                 \
	 INPUT_IMAGE_SIZE_PIXEL)
#define RUN_BOX_HEIGHT 40
#define RUN_BOX_Y (BSP_LCD_GetYSize() / 2 - (RUN_BOX_HEIGHT / 2))

static TS_StateTypeDef TS_State;
static uint16_t TSInputImage[INPUT_IMAGE_SIZE_PIXEL][INPUT_IMAGE_SIZE_PIXEL] = {
	0
};
static uint8_t NNInputImage[INPUT_IMAGE_SIZE * INPUT_IMAGE_SIZE];
static const uint32_t image_size = INPUT_IMAGE_SIZE;
static const uint32_t image_size_pixel = INPUT_IMAGE_SIZE_PIXEL;

uint16_t AverageImageBlock(uint32_t block_size, uint32_t x, uint32_t y)
{
	uint32_t ret[3] = { 0 };

	for (int i = 0; i < block_size; i++)
		for (int j = 0; j < block_size; j++) {
			ret[0] += TSInputImage[y * block_size + i]
					      [x * block_size + j] &
				  0x1F;
			ret[1] += (TSInputImage[y * block_size + i]
					       [x * block_size + j] >>
				   5) &
				  0x3F;
			ret[2] += (TSInputImage[y * block_size + i]
					       [x * block_size + j] >>
				   11) &
				  0x1F;
		}

	return (((ret[0] / (block_size * block_size)) & 0x1F) |
		(((ret[1] / (block_size * block_size)) & 0x3F) << 5) |
		(((ret[2] / (block_size * block_size)) & 0x1F) << 11));
}

void ShrinkImage(void)
{
	uint8_t scale_factor = image_size_pixel / image_size;

	for (int y = 0; y < image_size; y++)
		for (int x = 0; x < image_size; x++) {
			TSInputImage[y][x] =
				AverageImageBlock(scale_factor, x, y);
		}
}

void ConvertGStoTrueColor(void)
{
	static uint16_t tmp_pixel;
	static const int divisor = 256 / 32;
	for (int y = 0; y < image_size; y++)
		for (int x = 0; x < image_size; x++) {
			tmp_pixel = TSInputImage[y][x];
			tmp_pixel = (((((tmp_pixel >> 11) & 0x1F) +
				       ((tmp_pixel >> 5) & 0x1F) +
				       (tmp_pixel & 0x1F) + 3) /
				      3 * divisor) -
				     1) &
				    0xFF;
			TSInputImage[y][x] = tmp_pixel;
		}
}

static void SendImageUART(void)
{
	char buffer[INPUT_IMAGE_SIZE * 2 + 1] = { '\0' };
    printf("**********IMAGE START**********\n");
	for (int i = 0; i < image_size; i++) {
		for (int j = 0; j < image_size; j++){
			buffer[j * 2] = TSInputImage[i][j];
            buffer[j * 2 + 1] = ' ';
        }
		printf("%s\n", buffer);
	}
    printf("**********IMAGE STOP**********\n");
}

static void DrawShrunkImage(void)
{
	BSP_LCD_Clear(LCD_COLOR_WHITE);
	for (int i = 0; i < INPUT_IMAGE_SIZE; i++)
		for (int j = 0; j < INPUT_IMAGE_SIZE; j++)
			BSP_LCD_DrawPixel(j, i, TSInputImage[i][j]);
}

static void SaveInputImage(void)
{
	volatile int margin = INPUT_BOX_Y_INNER;
	TSInputImage[0][0] = 1;
	for (int y = 0; y < image_size_pixel; y++) {
		for (int x = 0; x < image_size_pixel; x++) {
			TSInputImage[y][x] =
				BSP_LCD_ReadPixel(x + margin, y + margin);
		}
	}

	ShrinkImage();

	DrawShrunkImage();

	ConvertGStoTrueColor();
	
    SendImageUART();

    for(int i = 0; i < INPUT_IMAGE_SIZE; i++)
        memcpy(&NNInputImage[i], &TSInputImage[i], INPUT_IMAGE_SIZE * sizeof(uint8_t));
}

uint8_t RunButtonPressed(uint32_t x, uint32_t y)
{
	if ((x >= RUN_BOX_X) && (x <= (RUN_BOX_X + RUN_BOX_WIDTH)) &&
	    (y >= RUN_BOX_Y) && (y <= (RUN_BOX_Y + RUN_BOX_HEIGHT)))
		return 1;

	return 0;
}

uint8_t DrawInBox(uint32_t x, uint32_t y, uint32_t dot_radius)
{
	if ((x < (INPUT_BOX_X_INNER + dot_radius)) ||
	    (x > (INPUT_BOX_X_INNER + INPUT_IMAGE_SIZE_PIXEL - dot_radius))) {
		return 0;
	}
	if ((y < (INPUT_BOX_Y_INNER + dot_radius)) ||
	    (y > (INPUT_BOX_Y_INNER + INPUT_IMAGE_SIZE_PIXEL - dot_radius))) {
		return 0;
	}

	return 1;
}

void DrawTouchInput(void)
{
	static const uint32_t dot_radius = 2;
	static uint32_t x = 0, y = 0;
    static char ret_char;

	BSP_TS_GetState(&TS_State);

	if (TS_State.TouchDetected) {
		x = TS_State.x;
		y = TS_State.y;

		if (DrawInBox(x, y, dot_radius)) {
			BSP_LCD_FillCircle(x, y, dot_radius);
		} else if (RunButtonPressed(x, y)) {
			printf("Save image\n");
			SaveInputImage();
            ret_char = loop((uint8_t*)NNInputImage, INPUT_IMAGE_SIZE * INPUT_IMAGE_SIZE);
		}
	}
}

void DrawInputScreen(void)
{
	BSP_LCD_SetFont(&Font16);
	BSP_LCD_Clear(LCD_COLOR_WHITE);
	BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
	BSP_LCD_FillRect(INPUT_BOX_X_OUTER, INPUT_BOX_Y_OUTER,
			 INPUT_IMAGE_SIZE_PIXEL + 2 * INPUT_BOX_THICKNESS,
			 INPUT_IMAGE_SIZE_PIXEL + 2 * INPUT_BOX_THICKNESS);
	BSP_LCD_FillRect(RUN_BOX_X, RUN_BOX_Y, RUN_BOX_WIDTH, RUN_BOX_HEIGHT);
	BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
	BSP_LCD_FillRect(INPUT_BOX_X_INNER, INPUT_BOX_Y_INNER,
			 INPUT_IMAGE_SIZE_PIXEL, INPUT_IMAGE_SIZE_PIXEL);
	BSP_LCD_FillRect(RUN_BOX_X + INPUT_BOX_THICKNESS,
			 RUN_BOX_Y + INPUT_BOX_THICKNESS,
			 RUN_BOX_WIDTH - 2 * INPUT_BOX_THICKNESS,
			 RUN_BOX_HEIGHT - 2 * INPUT_BOX_THICKNESS);
	BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
	BSP_LCD_DisplayStringAt(RUN_BOX_X + 20, RUN_BOX_Y + 12,
				(uint8_t *)"RUN", LEFT_MODE);
}

/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used 
  * @retval None
  */
void StartDefaultTask(void const *argument)
{
	DrawInputScreen();
	printf("Started\n");
    setup();
	BSP_LCD_Clear(LCD_COLOR_WHITE);
	for (;;) {
		DrawTouchInput();
		BSP_LCD_SetFont(&Font16);
		// BSP_LCD_Clear(LCD_COLOR_WHITE);
		BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
		HAL_Delay(1);
	}
}

/**
  * @brief  Function implementing the cameraTask thread.
  * @param  argument: Not used 
  * @retval None
  */
void StartCameraTask(void const *argument)
{
	// BSP_CAMERA_SnapshotStart((uint8_t*)CAMERA_FRAME_BUFFER);
	// BSP_CAMERA_Suspend();
	printf("Started\n");
	BSP_LCD_Clear(LCD_COLOR_WHITE);
	BSP_LCD_SetBackColor(LCD_COLOR_WHITE); 
    BSP_LCD_SetTextColor(LCD_COLOR_BLACK);
	BSP_LCD_DisplayStringAt(0, 100, (uint8_t*)"ello thre yall", CENTER_MODE);
	
	for (;;) {
		// BSP_CAMERA_Resume();
		// BSP_CAMERA_Suspend();
	}
}

/**
* @brief Function implementing the blinkTaskHandle thread.
* @param argument: Not used
* @retval None
*/
void blink(void const *argument)
{
	int count = 0;

	HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(LED4_GPIO_Port, LED4_Pin, GPIO_PIN_RESET);

	for (;;) {
		count %= 10;

		switch (count++) {
		case 0:
			HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin,
					  GPIO_PIN_SET);
			break;
		case 1:
			HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin,
					  GPIO_PIN_SET);
			break;
		case 2:
			HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin,
					  GPIO_PIN_SET);
			break;
		case 3:
			/** HAL_GPIO_WritePin(LED4_GPIO_Port, LED4_Pin, */
			/**           GPIO_PIN_SET); */
			break;
		case 5:
			HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin,
					  GPIO_PIN_RESET);
			break;
		case 6:
			HAL_GPIO_WritePin(LED2_GPIO_Port, LED2_Pin,
					  GPIO_PIN_RESET);
			break;
		case 7:
			HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin,
					  GPIO_PIN_RESET);
			break;
		case 8:
			/** HAL_GPIO_WritePin(LED4_GPIO_Port, LED4_Pin, */
			/**           GPIO_PIN_RESET); */
			break;
		default:
			break;
		}

		HAL_Delay(100);
	}
}

int main(void)
{
	/* MCU Configuration--------------------------------------------------------*/
	/* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	HAL_Init();

	/* Initialize the LCD */
	BSP_LCD_Init();

	/* Enable the LCD */
	BSP_LCD_DisplayOn();
	BSP_LCD_Clear(LCD_COLOR_WHITE);

	// Touchscreen_Calibration();
	// BSP_TS_Init(BSP_LCD_GetXSize(), BSP_LCD_GetYSize());

	// /* Clear the LCD Background layer */
	// BSP_LCD_Clear(LCD_COLOR_WHITE);

	// /** Touchscreen_Calibration(); */
	// BSP_TS_Init(BSP_LCD_GetXSize(), BSP_LCD_GetYSize());

	/*Link the SD Card disk I/O driver ###################################*/
	// if (FATFS_LinkDriver(&SD_Driver, SDPath) != 0) { 
	//     Error_Handler(); 
	// } 

	/* Create a FAT file system (format) on the logical drive */
	// f_mkfs((TCHAR const *)SDPath, FM_ANY, 0, buffer, sizeof(buffer)); 

	// /*##-4- Register the file system object to the FatFs module ################*/
	// if (f_mount(&SDFatFs, (TCHAR const *)SDPath, 0) != FR_OK) { 
	//     Error_Handler(); 
	// }
    BSP_CAMERA_Init(RESOLUTION_R320x240);

    // BSP_CAMERA_SnapshotStart((uint8_t*)CAMERA_FRAME_BUFFER);

	/* Configure the system clock */
	SystemClock_Config();

	UartHandle.Instance = USARTx;
	UartHandle.Init.BaudRate = 9600;
	UartHandle.Init.WordLength = UART_WORDLENGTH_8B;
	UartHandle.Init.StopBits = UART_STOPBITS_1;
	UartHandle.Init.Parity = UART_PARITY_ODD;
	UartHandle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	UartHandle.Init.Mode = UART_MODE_TX_RX;
	UartHandle.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&UartHandle) != HAL_OK) {
		/* Initialization Error */
		Error_Handler();
	}

	printf("UART online\n");

	/* Initialize all configured peripherals */
	MX_GPIO_Init();

	// osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 2000);
	// defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

	osThreadDef(cameraTask, StartCameraTask, osPriorityNormal, 0, 4000);
	cameraTaskHandle = osThreadCreate(osThread(cameraTask), NULL);

	osThreadDef(blinkTaskHandle, blink, osPriorityHigh, 0, 256);
	blinkTaskHandle = osThreadCreate(osThread(blinkTaskHandle), NULL);

	/* Start scheduler */
	osKernelStart();
	while (1) {
	}
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Configure the main internal regulator output voltage 
           *   */
	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
	/** Initializes the CPU, AHB and APB busses clocks 
               *   */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	RCC_OscInitStruct.PLL.PLLM = 8;
	RCC_OscInitStruct.PLL.PLLN = 168;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 7;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}
	/** Initializes the CPU, AHB and APB busses clocks 
                                   *   */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
				      RCC_CLOCKTYPE_SYSCLK |
				      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) !=
	    HAL_OK) {
		Error_Handler();
	}
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOE_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOI_CLK_ENABLE();
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOF_CLK_ENABLE();

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOG, LED2_Pin | LED1_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(LED4_GPIO_Port, LED4_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pins : FSMC_NE1_Pin FSMC_NWAIT_Pin D2_Pin FSMC_NWE_Pin 
                           D3_Pin FSMC_NOE_Pin FSMC_CLK_Pin D1_Pin 
                           D0_Pin A18_Pin A17_Pin A16_Pin 
                           D15_Pin D14_Pin D13_Pin */
	GPIO_InitStruct.Pin = FSMC_NE1_Pin | FSMC_NWAIT_Pin | D2_Pin |
			      FSMC_NWE_Pin | D3_Pin | FSMC_NOE_Pin |
			      FSMC_CLK_Pin | D1_Pin | D0_Pin | A18_Pin |
			      A17_Pin | A16_Pin | D15_Pin | D14_Pin | D13_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF12_FSMC;
	HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

	/*Configure GPIO pin : FSMC_NL_Pin */
	GPIO_InitStruct.Pin = FSMC_NL_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF12_FSMC;
	HAL_GPIO_Init(FSMC_NL_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pin : User_Button_Pin */
	GPIO_InitStruct.Pin = User_Button_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(User_Button_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pins : LED2_Pin LED1_Pin */
	GPIO_InitStruct.Pin = LED2_Pin | LED1_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

	/*Configure GPIO pins : FSMC_NE3_Pin FSMC_NE2_Pin A15_Pin A14_Pin 
                           A13_Pin A12_Pin A11_Pin A10_Pin */
	GPIO_InitStruct.Pin = FSMC_NE3_Pin | FSMC_NE2_Pin | A15_Pin | A14_Pin |
			      A13_Pin | A12_Pin | A11_Pin | A10_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF12_FSMC;
	HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

	/*Configure GPIO pin : LED3_Pin */
	GPIO_InitStruct.Pin = LED3_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LED3_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pin : LED4_Pin */
	GPIO_InitStruct.Pin = LED4_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(LED4_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pin : SW1_Pin */
	GPIO_InitStruct.Pin = SW1_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(SW1_GPIO_Port, &GPIO_InitStruct);
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
	while (1) {
	}
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif /* USE_FULL_ASSERT */
