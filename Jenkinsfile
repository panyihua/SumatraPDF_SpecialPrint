pipeline {
  agent any
  stages {
    stage('build') {
      steps {
        echo 'hello'
        sh 'echo world'
      }
    }

    stage('curl') {
      steps {
        sh 'curl www.baidu.com'
      }
    }

  }
}