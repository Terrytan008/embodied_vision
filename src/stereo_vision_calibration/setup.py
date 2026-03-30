from setuptools import setup

setup(
    name='stereo_vision_calibration',
    version='0.1.0',
    packages=['scripts'],
    package_dir={'': 'scripts'},
    install_requires=[
        'opencv-python',
        'numpy',
        'torch',
    ],
    zip_safe=False,
)
