from sklearn import neural_network as nn
import numpy as np
from PIL import Image
from random import shuffle
import sys
import os
import time
import signal

IS_TRAINING = True

def handle_sig_done(*args):
    global IS_TRAINING
    IS_TRAINING = False
    print("Ending training")

signal.signal(signal.SIGINT, handle_sig_done)

def activation_map(model, in_path, out_path):
    img = Image.open(in_path).convert('RGB')
    img_array = np.array(img)
    w, h, d = img_array.shape
    act_map = np.zeros((w - 32, h - 32, 3))

    px_h = h - 32

    for y in range(0, px_h):
        if (y * 100 // px_h) % 10 == 0:
            print(str(int(100 * y / px_h)) + '%')

        for x in range(0, w - 32):
            patch = img_array[x:x+32, y:y+32]
            flat_patch = (patch.flatten().reshape((1, 3072)) / 255.0) - 0.5
        
            _y = model.predict(flat_patch)[0]
            color = np.array([[[1, 1, 1]]])

            if _y[1] == 1:
                color = np.array([[[1, 0, 0]]])
            if _y[2] == 1:
                color = np.array([[[0, 1, 0]]])

            act_map[x, y] = patch[16, 16] * color

    Image.fromarray(act_map.astype(np.uint8)).save(out_path, "PNG")


def filenames_labels():
    files_labels = []

    for f in os.listdir('imgs/0'):
        files_labels += [('imgs/0/' + f, [1, 0, 0])]

    for f in os.listdir('imgs/1'):
        files_labels += [('imgs/1/' + f, [0, 1, 0])]

    for f in os.listdir('imgs/2'):
        files_labels += [('imgs/2/' + f, [0, 0, 1])]

    shuffle(files_labels)

    return files_labels


def real_test_filenames_labels():
    return [
        ('imgs/real/real0', [0,1,0]),
        ('imgs/real/real1', [0,1,0])
    ]


def array_from_file(path):
    img = Image.open(path).convert('RGB')
    return np.array(img)


def show_example(X, i, title=None):
    test = Image.fromarray(((X[i] + 0.5) * 256).reshape((32, 32, 3)).astype(np.uint8))
    test.show(title=title)


def minibatch(files_labels, index, size=100):
    X, Y = [], []

    for file, label in files_labels[index * size: index * size + size]:
        img_array = array_from_file(file)
        assert(img_array.size == 3 * 32**2)

        X += [img_array.flatten()]
        Y += [label]

    m = len(X)

    return (np.array(X).reshape((m, 3072)) / 256.0 - 0.5), np.array(Y).reshape((m, 3))


def main(argv):
    # train({
    #     'layer_sizes': [256, 256, 256],
    #     'learning_rate': 0.0001,
    #     'l2_reg_term': 0.001,
    #     'epochs': -1             # -1: only stops at SIGINT
    # })

    hyper_parameter_search({
        'layer_sizes': ([64, 512], 1, 5),
        'learning_rate': (0.0, 0.0001, 0.001),
        'l2_reg_term': (0.0, 0.0001, 0.001)
    })


def hyper_parameter_search(ranges, candidates=100):

    hyper_param_set = []

    for _ in range(candidates):
        hyper_params = {}

        for param in ranges:
            t, low, high = ranges[param]
            rnd = low + np.random.random() * (high - low)

            if type(t) is int:
                rnd = int(rnd)
            elif type(t) is float:
                rnd = float(rnd)
            elif type(t) is list:
                layer_size = int(np.random.random() * (t[1] - t[0]) + t[0])
                rnd = [layer_size] * int(np.ceil(rnd))

            hyper_params[param] = rnd

        hyper_params['epochs'] = 10
        hyper_param_set.append(hyper_params)

    i = 0
    best = 0
    for hp in hyper_param_set:
        print('%d/100 evaluated' % i)
        hp['dev_score'] = train(hp)
        i += 1

        if hp['dev_score'] > best:
            best = hp['dev_score']
            print(hp)


def train(hyper_params):
    # Get the set of all the labels and file paths, pre shuffled
    full_set = filenames_labels()

    # find total number of samples, compute count of complete batches
    m = len(full_set)
    n = 1000 # mini batch size
    batches = m // 1000

    # Split in to a training and test set
    ts_batches = int(np.floor(batches * 0.75)) - 1
    training_set = full_set[0:ts_batches * n]
    dev_set = full_set[ts_batches * n:-1]

    model = nn.MLPClassifier(hidden_layer_sizes=hyper_params['layer_sizes'],
                             learning_rate_init=hyper_params['learning_rate'],
                             alpha=hyper_params['l2_reg_term'],
                             solver='adam',
                             max_iter=2,
                             warm_start=True)

    epochs = hyper_params['epochs']
    while IS_TRAINING or epochs == 0:
        epochs -= 1
        for i in range(ts_batches):
            if not IS_TRAINING:
                break

            X, Y = minibatch(training_set, i, size=n)
            model.fit(X, Y)

            if i % 10 == 0:
                print(model.score(X, Y))


    ds_X, ds_Y = minibatch(dev_set, 0, size=100)
    score = model.score(ds_X, ds_Y)
    print('Dev set score: %f' % score)

    # activation_map(model, "test0.png", "act_map0.png")
    # activation_map(model, "test1.png", "act_map1.png")

    #X, Y = minibatch(real_test_filenames_labels(), 0, size=2)
    #print('Real set score: %f' % model.score(X, Y))

    # X, Y = minibatch(real_test_filenames_labels(), 0, size=2)
    # print('Real set score: %f' % model.score(X, Y))
    # print(model.predict(X))

    return score

if __name__ == '__main__':
    main(sys.argv)
