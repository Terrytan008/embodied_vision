from setuptools import setup, find_packages
from glob import glob
import os

setup(
    name='stereo_vision_calibration',
    version='0.1.0',
    packages=find_packages(include=['scripts', 'scripts.*']),
    py_modules=[
        'scripts.online_calibrator',
        'scripts.train_confidence',
        'scripts.export_onnx',
    ],
    install_requires=[
        'opencv-python',
        'numpy',
        'torch',
    ],
    zip_safe=False,
    package_data={
        'scripts': ['**/*.py'],
    },
    include_package_data=True,
)
