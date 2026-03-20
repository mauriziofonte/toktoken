import service.Processor

case class Job(id: String, payload: String, priority: Int = 0)
case class Result(jobId: String, output: String, success: Boolean)

object App {
  val version: String = "2.0.0"

  def main(args: Array[String]): Unit = {
    println(s"App v$version starting...")
    val jobs = args.map(arg => Job(id = s"job-${arg.hashCode.abs}", payload = arg))
    run(jobs.toList)
  }

  def run(jobs: List[Job]): Unit = {
    val processor = new Processor()

    val results = jobs.map { job =>
      processor.validate(job.payload) match {
        case Some(error) =>
          println(s"Validation failed for ${job.id}: $error")
          Result(job.id, error, success = false)
        case None =>
          val output = processor.execute(job.payload)
          println(s"Processed ${job.id}: $output")
          Result(job.id, output, success = true)
      }
    }

    val (succeeded, failed) = results.partition(_.success)
    println(s"Completed: ${succeeded.length} succeeded, ${failed.length} failed")
  }
}
