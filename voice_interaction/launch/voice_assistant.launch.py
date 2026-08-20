import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterFile


def generate_launch_description():
    default_config_file = os.path.join(
        get_package_share_directory('voice_interaction'),
        'config',
        'voice_assistant.yaml',
    )
    config_file = LaunchConfiguration('config_file')

    return LaunchDescription([
        DeclareLaunchArgument(
            'config_file',
            default_value=default_config_file,
            description='Path to the voice assistant ROS 2 parameter file',
        ),
        Node(
            package='respeaker_driver',
            executable='respeaker_node',
            name='respeaker_node',
            parameters=[ParameterFile(config_file, allow_substs=True)],
        ),
        Node(
            package='voice_interaction',
            executable='interaction_node',
            name='voice_interaction_node',
            parameters=[ParameterFile(config_file, allow_substs=True)],
        ),
    ])
