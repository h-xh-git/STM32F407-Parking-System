[basic]
type = cvimodel
model = ch_PP_OCRv3_det_int8.cvimodel

[extra]
model_type = pp_ocr
input_type = bgr
det = true
mean = 123.675, 116.28, 103.53
scale = 0.01712475, 0.017507, 0.01742919
rec_model = ch_PP_OCRv4_rec_int8.cvimodel
rec_mean = 127.5, 127.5, 127.5
rec_scale = 0.00784313725490196, 0.00784313725490196, 0.00784313725490196
labels = ppocr_keys_v1.txt




